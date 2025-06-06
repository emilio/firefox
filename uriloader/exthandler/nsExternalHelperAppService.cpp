/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim:expandtab:shiftwidth=2:tabstop=2:cin:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "base/basictypes.h"

/* This must occur *after* base/basictypes.h to avoid typedefs conflicts. */
#include "mozilla/ArrayUtils.h"
#include "mozilla/Base64.h"
#include "mozilla/ResultExtensions.h"

#include "mozilla/dom/ContentChild.h"
#include "mozilla/dom/BrowserChild.h"
#include "mozilla/dom/CanonicalBrowsingContext.h"
#include "mozilla/dom/Document.h"
#include "mozilla/dom/Element.h"
#include "mozilla/dom/WindowGlobalParent.h"
#include "mozilla/RandomNum.h"
#include "mozilla/StaticPrefs_dom.h"
#include "mozilla/StaticPrefs_security.h"
#include "mozilla/StaticPtr.h"
#include "nsXULAppAPI.h"

#include "ExternalHelperAppParent.h"
#include "nsExternalHelperAppService.h"
#include "nsCExternalHandlerService.h"
#include "nsIURI.h"
#include "nsIURL.h"
#include "nsIFile.h"
#include "nsIFileURL.h"
#include "nsIChannel.h"
#include "nsAppDirectoryServiceDefs.h"
#include "nsICategoryManager.h"
#include "nsDependentSubstring.h"
#include "nsSandboxFlags.h"
#include "nsString.h"
#include "nsUnicharUtils.h"
#include "nsIStringEnumerator.h"
#include "nsIStreamListener.h"
#include "nsIMIMEService.h"
#include "nsILoadGroup.h"
#include "nsIWebProgressListener.h"
#include "nsITransfer.h"
#include "nsReadableUtils.h"
#include "nsIRequest.h"
#include "nsDirectoryServiceDefs.h"
#include "nsIInterfaceRequestor.h"
#include "nsThreadUtils.h"
#include "nsIMutableArray.h"
#include "nsIRedirectHistoryEntry.h"
#include "nsOSHelperAppService.h"
#include "nsOSHelperAppServiceChild.h"
#include "nsContentSecurityUtils.h"
#include "nsUTF8Utils.h"
#include "nsUnicodeProperties.h"

// used to access our datastore of user-configured helper applications
#include "nsIHandlerService.h"
#include "nsIMIMEInfo.h"
#include "nsIHelperAppLauncherDialog.h"
#include "nsIContentDispatchChooser.h"
#include "nsNetUtil.h"
#include "nsIPrivateBrowsingChannel.h"
#include "nsIIOService.h"
#include "nsNetCID.h"

#include "nsIApplicationReputation.h"

#include "nsDSURIContentListener.h"
#include "nsMimeTypes.h"
#include "nsMIMEInfoImpl.h"
// used for header disposition information.
#include "nsIHttpChannel.h"
#include "nsIHttpChannelInternal.h"
#include "nsIEncodedChannel.h"
#include "nsIMultiPartChannel.h"
#include "nsIFileChannel.h"
#include "nsIObserverService.h"  // so we can be a profile change observer
#include "nsIPropertyBag2.h"     // for the 64-bit content length

#ifdef XP_MACOSX
#  include "nsILocalFileMac.h"
#endif

#include "nsEscape.h"

#include "nsIStringBundle.h"  // XXX needed to localize error msgs
#include "nsIPrompt.h"

#include "nsITextToSubURI.h"  // to unescape the filename

#include "nsDocShellCID.h"

#include "nsCRT.h"
#include "nsLocalHandlerApp.h"

#include "nsIRandomGenerator.h"

#include "ContentChild.h"
#include "nsXULAppAPI.h"
#include "nsPIDOMWindow.h"
#include "ExternalHelperAppChild.h"

#include "mozilla/dom/nsHTTPSOnlyUtils.h"

#ifdef XP_WIN
#  include "nsWindowsHelpers.h"
#  include "nsLocalFile.h"
#endif

#include "mozilla/Components.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/Preferences.h"
#include "mozilla/ipc/URIUtils.h"

using namespace mozilla;
using namespace mozilla::ipc;
using namespace mozilla::dom;

#define kDefaultMaxFileNameLength 254

// Download Folder location constants
#define NS_PREF_DOWNLOAD_DIR "browser.download.dir"
#define NS_PREF_DOWNLOAD_FOLDERLIST "browser.download.folderList"
enum {
  NS_FOLDER_VALUE_DESKTOP = 0,
  NS_FOLDER_VALUE_DOWNLOADS = 1,
  NS_FOLDER_VALUE_CUSTOM = 2
};

LazyLogModule nsExternalHelperAppService::sLog("HelperAppService");

// Using level 3 here because the OSHelperAppServices use a log level
// of LogLevel::Debug (4), and we want less detailed output here
// Using 3 instead of LogLevel::Warning because we don't output warnings
#undef LOG
#define LOG(...)                                                     \
  MOZ_LOG(nsExternalHelperAppService::sLog, mozilla::LogLevel::Info, \
          (__VA_ARGS__))
#define LOG_ENABLED() \
  MOZ_LOG_TEST(nsExternalHelperAppService::sLog, mozilla::LogLevel::Info)

static const char NEVER_ASK_FOR_SAVE_TO_DISK_PREF[] =
    "browser.helperApps.neverAsk.saveToDisk";
static const char NEVER_ASK_FOR_OPEN_FILE_PREF[] =
    "browser.helperApps.neverAsk.openFile";

StaticRefPtr<nsIFile> sFallbackDownloadDir;

// Helper functions for Content-Disposition headers

/**
 * Given a URI fragment, unescape it
 * @param aFragment The string to unescape
 * @param aURI The URI from which this fragment is taken. Only its character set
 *             will be used.
 * @param aResult [out] Unescaped string.
 */
static nsresult UnescapeFragment(const nsACString& aFragment, nsIURI* aURI,
                                 nsAString& aResult) {
  // We need the unescaper
  nsresult rv;
  nsCOMPtr<nsITextToSubURI> textToSubURI =
      do_GetService(NS_ITEXTTOSUBURI_CONTRACTID, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  return textToSubURI->UnEscapeURIForUI(aFragment, /* aDontEscape = */ true,
                                        aResult);
}

/**
 * UTF-8 version of UnescapeFragment.
 * @param aFragment The string to unescape
 * @param aURI The URI from which this fragment is taken. Only its character set
 *             will be used.
 * @param aResult [out] Unescaped string, UTF-8 encoded.
 * @note It is safe to pass the same string for aFragment and aResult.
 * @note When this function fails, aResult will not be modified.
 */
static nsresult UnescapeFragment(const nsACString& aFragment, nsIURI* aURI,
                                 nsACString& aResult) {
  nsAutoString result;
  nsresult rv = UnescapeFragment(aFragment, aURI, result);
  if (NS_SUCCEEDED(rv)) CopyUTF16toUTF8(result, aResult);
  return rv;
}

static Result<nsCOMPtr<nsIFile>, nsresult> GetOsTmpDownloadDirectory() {
  nsCOMPtr<nsIFile> dir;
  MOZ_TRY(NS_GetSpecialDirectory(NS_OS_TEMP_DIR, getter_AddRefs(dir)));

#if !defined(XP_MACOSX) && defined(XP_UNIX)
  // Ensuring that only the current user can read the file names we end up
  // creating. Note that creating directories with a specified permission is
  // only supported on Unix platform right now. That's why the above check
  // exists.

  uint32_t permissions;
  MOZ_TRY(dir->GetPermissions(&permissions));

  if (permissions != PR_IRWXU) {
    const char* userName = PR_GetEnv("USERNAME");
    if (!userName || !*userName) {
      userName = PR_GetEnv("USER");
    }
    if (!userName || !*userName) {
      userName = PR_GetEnv("LOGNAME");
    }
    if (!userName || !*userName) {
      userName = "mozillaUser";
    }

    nsAutoString userDir;
    userDir.AssignLiteral("mozilla_");
    userDir.AppendASCII(userName);
    userDir.ReplaceChar(u"" FILE_PATH_SEPARATOR FILE_ILLEGAL_CHARACTERS, '_');

    int counter = 0;
    bool pathExists;
    nsCOMPtr<nsIFile> finalPath;

    while (true) {
      nsAutoString countedUserDir(userDir);
      countedUserDir.AppendInt(counter, 10);
      dir->Clone(getter_AddRefs(finalPath));
      finalPath->Append(countedUserDir);

      MOZ_TRY(finalPath->Exists(&pathExists));

      if (pathExists) {
        // If this path has the right permissions, use it.
        MOZ_TRY(finalPath->GetPermissions(&permissions));

        // Ensuring the path is writable by the current user.
        bool isWritable;
        MOZ_TRY(finalPath->IsWritable(&isWritable));

        if (permissions == PR_IRWXU && isWritable) {
          dir = finalPath;
          break;
        }
      }

      nsresult const rv = finalPath->Create(nsIFile::DIRECTORY_TYPE, PR_IRWXU);
      if (NS_SUCCEEDED(rv)) {
        dir = finalPath;
        break;
      }
      if (rv != NS_ERROR_FILE_ALREADY_EXISTS) {
        // Unexpected error.
        return Err(rv);
      }
      counter++;
    }
  }

#endif
  NS_ASSERTION(dir, "Somehow we didn't get a download directory!");
  return dir;
}

/**
 * Given an alleged download directory, either create it, or confirm that it
 * already exists and is usable.
 */
static nsresult EnsureDirectoryExists(nsIFile* aDir) {
  nsresult const rv = aDir->Create(nsIFile::DIRECTORY_TYPE, 0755);
  if (rv == NS_ERROR_FILE_ALREADY_EXISTS || NS_SUCCEEDED(rv)) {
    return NS_OK;
  }
  return rv;
};

/**
 * Obtains the final directory to save downloads to. This tends to vary per
 * platform, and needs to be consistent throughout our codepaths. For platforms
 * where helper apps use the downloads directory, this should be kept in sync
 * with the function of the same name in DownloadIntegration.sys.mjs.
 *
 * Optionally skip availability of the directory and storage.
 */
static Result<nsCOMPtr<nsIFile>, nsresult> GetPreferredDownloadsDirectory(
    bool aSkipChecks = false) {
#if defined(ANDROID)
  return Err(NS_ERROR_FAILURE);
#endif

  nsresult rv;
  // Try to get the users download location, if it's set.
  switch (Preferences::GetInt(NS_PREF_DOWNLOAD_FOLDERLIST, -1)) {
    case NS_FOLDER_VALUE_DESKTOP: {
      nsCOMPtr<nsIFile> dir;
      if (NS_SUCCEEDED(
              NS_GetSpecialDirectory(NS_OS_DESKTOP_DIR, getter_AddRefs(dir)))) {
        return dir;
      }
    } break;

    case NS_FOLDER_VALUE_CUSTOM: {
      nsCOMPtr<nsIFile> dir;
      Preferences::GetComplex(NS_PREF_DOWNLOAD_DIR, NS_GET_IID(nsIFile),
                              getter_AddRefs(dir));
      if (!dir) break;

      // Check for availability if requested.
      if (!aSkipChecks && NS_FAILED(EnsureDirectoryExists(dir))) {
        break;
      }

      return dir;
    } break;

    default:
    case NS_FOLDER_VALUE_DOWNLOADS:
      // This is just the OS default location, so fall out
      break;
  }

  // Fallthrough: get OS default directory

  nsCOMPtr<nsIFile> dir;
  rv = NS_GetSpecialDirectory(NS_OS_DEFAULT_DOWNLOAD_DIR, getter_AddRefs(dir));
  if (NS_SUCCEEDED(rv)) {
    return dir;
  }

  // On some OSes, there is no guarantee that `NS_OS_DEFAULT_DOWNLOAD_DIR`
  // exists. Fall back to $HOME + Downloads.

  // If we've done this before, use the cached value:
  if (sFallbackDownloadDir) {
    MOZ_TRY(sFallbackDownloadDir->Clone(getter_AddRefs(dir)));
    return dir;
  }

  MOZ_TRY(NS_GetSpecialDirectory(NS_OS_HOME_DIR, getter_AddRefs(dir)));

  // Get the appropriate translation of "Downloads"...
  nsAutoString downloadLocalized;
  rv = [&downloadLocalized]() {
    nsresult rv;

    nsCOMPtr<nsIStringBundleService> bundleService =
        do_GetService(NS_STRINGBUNDLE_CONTRACTID, &rv);
    NS_ENSURE_SUCCESS(rv, rv);

    nsCOMPtr<nsIStringBundle> downloadBundle;
    rv = bundleService->CreateBundle(
        "chrome://mozapps/locale/downloads/downloads.properties",
        getter_AddRefs(downloadBundle));
    NS_ENSURE_SUCCESS(rv, rv);

    return downloadBundle->GetStringFromName("downloadsFolder",
                                             downloadLocalized);
  }();
  // ... or, failing that, just use "Downloads".
  if (NS_FAILED(rv)) {
    downloadLocalized.AssignLiteral("Downloads");
  }
  MOZ_TRY(dir->Append(downloadLocalized));

  // Cache the result for next time.
  {
    // Can't getter_AddRefs on StaticRefPtr, so do some copying.
    nsCOMPtr<nsIFile> copy;
    dir->Clone(getter_AddRefs(copy));
    sFallbackDownloadDir = copy.forget();
    ClearOnShutdown(&sFallbackDownloadDir);
  }

  // Check for availability if requested.
  if (!aSkipChecks) {
    MOZ_TRY(EnsureDirectoryExists(dir));
  }

  return dir;
}

NS_IMETHODIMP nsExternalHelperAppService::GetPreferredDownloadsDirectory(
    nsIFile** aOutFile) {
  auto res = ::GetPreferredDownloadsDirectory();
  if (res.isErr()) return res.unwrapErr();
  res.unwrap().forget(aOutFile);
  return NS_OK;
}

/**
 * Obtains the initial directory to save downloads to. (This may differ from the
 * actual download directory if "browser.download.start_downloads_in_tmp_dir" is
 * set.)
 *
 * Optionally, skip availability of the directory and storage.
 */
static Result<nsCOMPtr<nsIFile>, nsresult> GetInitialDownloadDirectory(
    bool aSkipChecks = false) {
#if defined(ANDROID)
  return Err(NS_ERROR_FAILURE);
#endif

  if (StaticPrefs::browser_download_start_downloads_in_tmp_dir()) {
    return GetOsTmpDownloadDirectory();
  }

  return GetPreferredDownloadsDirectory(aSkipChecks);
}

/**
 * Helper for random bytes for the filename of downloaded part files.
 */
nsresult GenerateRandomName(nsACString& result) {
  // We will request raw random bytes, and transform that to a base64 string,
  // using url-based base64 encoding so that all characters from the base64
  // result will be acceptable for filenames.
  // For each three bytes of random data, we will get four bytes of ASCII.
  // Request a bit more, to be safe, then truncate in the end.

  nsresult rv;
  const uint32_t wantedFileNameLength = 8;
  const uint32_t requiredBytesLength =
      static_cast<uint32_t>((wantedFileNameLength + 1) / 4 * 3);

  uint8_t buffer[requiredBytesLength];
  if (!mozilla::GenerateRandomBytesFromOS(buffer, requiredBytesLength)) {
    return NS_ERROR_FAILURE;
  }

  nsAutoCString tempLeafName;
  // We're forced to specify a padding policy, though this is guaranteed
  // not to need padding due to requiredBytesLength being a multiple of 3.
  rv = Base64URLEncode(requiredBytesLength, buffer,
                       Base64URLEncodePaddingPolicy::Omit, tempLeafName);
  NS_ENSURE_SUCCESS(rv, rv);

  tempLeafName.Truncate(wantedFileNameLength);

  result.Assign(tempLeafName);
  return NS_OK;
}

/**
 * Structure for storing extension->type mappings.
 * @see defaultMimeEntries
 */
struct nsDefaultMimeTypeEntry {
  const char* mMimeType;
  const char* mFileExtension;
};

/**
 * Default extension->mimetype mappings. These are not overridable.
 * If you add types here, make sure they are lowercase, or you'll regret it.
 */
static const nsDefaultMimeTypeEntry defaultMimeEntries[] = {
    // The following are those extensions that we're asked about during startup,
    // sorted by order used
    {TEXT_XML, "xml"},
    {IMAGE_PNG, "png"},
    // -- end extensions used during startup
    {TEXT_CSS, "css"},
    {IMAGE_JPEG, "jpeg"},
    {IMAGE_JPEG, "jpg"},
    {IMAGE_SVG_XML, "svg"},
    {TEXT_HTML, "html"},
    {TEXT_HTML, "htm"},
    {IMAGE_GIF, "gif"},
    {IMAGE_WEBP, "webp"},
    {APPLICATION_XPINSTALL, "xpi"},
    {APPLICATION_XHTML_XML, "xhtml"},
    {APPLICATION_XHTML_XML, "xht"},
    {TEXT_PLAIN, "txt"},
    {APPLICATION_JSON, "json"},
    {APPLICATION_RDF, "rdf"},
    {APPLICATION_XJAVASCRIPT, "mjs"},
    {APPLICATION_XJAVASCRIPT, "js"},
    {APPLICATION_XJAVASCRIPT, "jsm"},
    {VIDEO_OGG, "ogv"},
    {APPLICATION_OGG, "ogg"},
    {AUDIO_OGG, "oga"},
    {AUDIO_OGG, "opus"},
    {APPLICATION_PDF, "pdf"},
    {VIDEO_WEBM, "webm"},
    {AUDIO_WEBM, "webm"},
    {IMAGE_ICO, "ico"},
    {TEXT_PLAIN, "properties"},
    {TEXT_PLAIN, "locale"},
    {TEXT_PLAIN, "ftl"},
#if defined(MOZ_WMF)
    {VIDEO_MP4, "mp4"},
    {AUDIO_MP4, "m4a"},
    {AUDIO_MP3, "mp3"},
#endif
#ifdef MOZ_RAW
    {VIDEO_RAW, "yuv"}
#endif
};

/**
 * This is a small private struct used to help us initialize some
 * default mime types.
 */
struct nsExtraMimeTypeEntry {
  const char* mMimeType;
  const char* mFileExtensions;
  const char* mDescription;
};

/**
 * This table lists all of the 'extra' content types that we can deduce from
 * particular file extensions.  These entries also ensure that we provide a good
 * descriptive name when we encounter files with these content types and/or
 * extensions.  These can be overridden by user helper app prefs. If you add
 * types here, make sure they are lowercase, or you'll regret it.
 */
static const nsExtraMimeTypeEntry extraMimeEntries[] = {
#if defined(XP_MACOSX)  // don't define .bin on the mac...use internet config to
                        // look that up...
    {APPLICATION_OCTET_STREAM, "exe,com", "Binary File"},
#else
    {APPLICATION_OCTET_STREAM, "exe,com,bin", "Binary File"},
#endif
    {APPLICATION_GZIP2, "gz", "gzip"},
    {"application/x-arj", "arj", "ARJ file"},
    {"application/rtf", "rtf", "Rich Text Format File"},
    {APPLICATION_ZIP, "zip", "ZIP Archive"},
    {APPLICATION_XPINSTALL, "xpi", "XPInstall Install"},
    {APPLICATION_PDF, "pdf", "Portable Document Format"},
    {APPLICATION_POSTSCRIPT, "ps,eps,ai", "Postscript File"},
    {APPLICATION_XJAVASCRIPT, "js", "Javascript Source File"},
    {APPLICATION_XJAVASCRIPT, "jsm,mjs", "Javascript Module Source File"},
#ifdef MOZ_WIDGET_ANDROID
    {"application/vnd.android.package-archive", "apk", "Android Package"},
#endif

    // OpenDocument formats
    {"application/vnd.oasis.opendocument.text", "odt", "OpenDocument Text"},
    {"application/vnd.oasis.opendocument.presentation", "odp",
     "OpenDocument Presentation"},
    {"application/vnd.oasis.opendocument.spreadsheet", "ods",
     "OpenDocument Spreadsheet"},
    {"application/vnd.oasis.opendocument.graphics", "odg",
     "OpenDocument Graphics"},

    // Legacy Microsoft Office
    {"application/msword", "doc", "Microsoft Word"},
    {"application/vnd.ms-powerpoint", "ppt", "Microsoft PowerPoint"},
    {"application/vnd.ms-excel", "xls", "Microsoft Excel"},

    // Office Open XML
    {"application/vnd.openxmlformats-officedocument.wordprocessingml.document",
     "docx", "Microsoft Word (Open XML)"},
    {"application/"
     "vnd.openxmlformats-officedocument.presentationml.presentation",
     "pptx", "Microsoft PowerPoint (Open XML)"},
    {"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
     "xlsx", "Microsoft Excel (Open XML)"},

    {IMAGE_ART, "art", "ART Image"},
    {IMAGE_BMP, "bmp", "BMP Image"},
    {IMAGE_GIF, "gif", "GIF Image"},
    {IMAGE_ICO, "ico,cur", "ICO Image"},
    {IMAGE_JPEG, "jpg,jpeg,jfif,pjpeg,pjp", "JPEG Image"},
    {IMAGE_PNG, "png", "PNG Image"},
    {IMAGE_APNG, "apng", "APNG Image"},
    {IMAGE_TIFF, "tiff,tif", "TIFF Image"},
    {IMAGE_XBM, "xbm", "XBM Image"},
    {IMAGE_SVG_XML, "svg", "Scalable Vector Graphics"},
    {IMAGE_WEBP, "webp", "WebP Image"},
    {IMAGE_AVIF, "avif", "AV1 Image File"},
    {IMAGE_JXL, "jxl", "JPEG XL Image File"},

    {MESSAGE_RFC822, "eml", "RFC-822 data"},
    {TEXT_PLAIN, "txt,text", "Text File"},
    {APPLICATION_JSON, "json", "JavaScript Object Notation"},
    {TEXT_VTT, "vtt", "Web Video Text Tracks"},
    {TEXT_CACHE_MANIFEST, "appcache", "Application Cache Manifest"},
    {TEXT_HTML, "html,htm,shtml,ehtml", "HyperText Markup Language"},
    {"application/xhtml+xml", "xhtml,xht",
     "Extensible HyperText Markup Language"},
    {APPLICATION_MATHML_XML, "mml", "Mathematical Markup Language"},
    {APPLICATION_RDF, "rdf", "Resource Description Framework"},
    {"text/csv", "csv", "CSV File"},
    {TEXT_XML, "xml,xsl,xbl", "Extensible Markup Language"},
    {TEXT_CSS, "css", "Style Sheet"},
    {TEXT_VCARD, "vcf,vcard", "Contact Information"},
    {TEXT_CALENDAR, "ics,ical,ifb,icalendar", "iCalendar"},
    {VIDEO_OGG, "ogv,ogg", "Ogg Video"},
    {APPLICATION_OGG, "ogg", "Ogg Video"},
    {AUDIO_OGG, "oga", "Ogg Audio"},
    {AUDIO_OGG, "opus", "Opus Audio"},
    {VIDEO_WEBM, "webm", "Web Media Video"},
    {AUDIO_WEBM, "webm", "Web Media Audio"},
    {AUDIO_MP3, "mp3,mpega,mp2", "MPEG Audio"},
    {VIDEO_MP4, "mp4,m4a,m4b", "MPEG-4 Video"},
    {AUDIO_MP4, "m4a,m4b", "MPEG-4 Audio"},
    {VIDEO_RAW, "yuv", "Raw YUV Video"},
    {AUDIO_WAV, "wav", "Waveform Audio"},
    {VIDEO_3GPP, "3gpp,3gp", "3GPP Video"},
    {VIDEO_3GPP2, "3g2", "3GPP2 Video"},
    {AUDIO_AAC, "aac", "AAC Audio"},
    {AUDIO_FLAC, "flac", "FLAC Audio"},
    {AUDIO_MIDI, "mid", "Standard MIDI Audio"},
    {APPLICATION_WASM, "wasm", "WebAssembly Module"},
    {"application/epub+zip", "epub", "Electronic publication (EPUB)"}};

static const nsDefaultMimeTypeEntry sForbiddenPrimaryExtensions[] = {
    {IMAGE_JPEG, "jfif"}};

/**
 * File extensions for which decoding should be disabled.
 * NOTE: These MUST be lower-case and ASCII.
 */
static const nsDefaultMimeTypeEntry nonDecodableExtensions[] = {
    {APPLICATION_GZIP, "gz"},
    {APPLICATION_GZIP, "tgz"},
    {APPLICATION_ZIP, "zip"},
    {APPLICATION_COMPRESS, "z"},
    {APPLICATION_GZIP, "svgz"}};

/**
 * Mimetypes for which we enforce using a known extension.
 *
 * In addition to this list, we do this for all audio/, video/ and
 * image/ mimetypes.
 */
static const char* forcedExtensionMimetypes[] = {
    APPLICATION_PDF, APPLICATION_OGG, APPLICATION_WASM,
    TEXT_CALENDAR,   TEXT_CSS,        TEXT_VCARD};

/**
 * Primary extensions of types whose descriptions should be overwritten.
 * This extension is concatenated with "ExtHandlerDescription" to look up the
 * description in unknownContentType.properties.
 * NOTE: These MUST be lower-case and ASCII.
 */
static const char* descriptionOverwriteExtensions[] = {
    "avif", "jxl", "pdf", "svg", "webp", "xml",
};

static StaticRefPtr<nsExternalHelperAppService> sExtHelperAppSvcSingleton;

/**
 * In child processes, return an nsOSHelperAppServiceChild for remoting
 * OS calls to the parent process.  In the parent process itself, use
 * nsOSHelperAppService.
 */
/* static */
already_AddRefed<nsExternalHelperAppService>
nsExternalHelperAppService::GetSingleton() {
  if (!sExtHelperAppSvcSingleton) {
    if (XRE_IsParentProcess()) {
      sExtHelperAppSvcSingleton = new nsOSHelperAppService();
    } else {
      sExtHelperAppSvcSingleton = new nsOSHelperAppServiceChild();
    }
    ClearOnShutdown(&sExtHelperAppSvcSingleton);
  }

  return do_AddRef(sExtHelperAppSvcSingleton);
}

NS_IMPL_ISUPPORTS(nsExternalHelperAppService, nsIExternalHelperAppService,
                  nsPIExternalAppLauncher, nsIExternalProtocolService,
                  nsIMIMEService, nsIObserver, nsISupportsWeakReference)

nsExternalHelperAppService::nsExternalHelperAppService() {}
nsresult nsExternalHelperAppService::Init() {
  // Add an observer for profile change
  nsCOMPtr<nsIObserverService> obs = mozilla::services::GetObserverService();
  if (!obs) return NS_ERROR_FAILURE;

  nsresult rv = obs->AddObserver(this, "profile-before-change", true);
  NS_ENSURE_SUCCESS(rv, rv);
  return obs->AddObserver(this, "last-pb-context-exited", true);
}

nsExternalHelperAppService::~nsExternalHelperAppService() {}

nsresult nsExternalHelperAppService::DoContentContentProcessHelper(
    const nsACString& aMimeContentType, nsIChannel* aChannel,
    BrowsingContext* aContentContext, bool aForceSave,
    nsIInterfaceRequestor* aWindowContext,
    nsIStreamListener** aStreamListener) {
  NS_ENSURE_ARG_POINTER(aChannel);

  // We need to get a hold of a ContentChild so that we can begin forwarding
  // this data to the parent.  In the HTTP case, this is unfortunate, since
  // we're actually passing data from parent->child->parent wastefully, but
  // the Right Fix will eventually be to short-circuit those channels on the
  // parent side based on some sort of subscription concept.
  using mozilla::dom::ContentChild;
  using mozilla::dom::ExternalHelperAppChild;
  ContentChild* child = ContentChild::GetSingleton();
  if (!child) {
    return NS_ERROR_FAILURE;
  }

  nsCString disp;
  nsCOMPtr<nsIURI> uri;
  int64_t contentLength = -1;
  bool wasFileChannel = false;
  uint32_t contentDisposition = -1;
  nsAutoString fileName;
  nsCOMPtr<nsILoadInfo> loadInfo;

  aChannel->GetURI(getter_AddRefs(uri));
  aChannel->GetContentLength(&contentLength);
  aChannel->GetContentDisposition(&contentDisposition);
  aChannel->GetContentDispositionFilename(fileName);
  aChannel->GetContentDispositionHeader(disp);
  loadInfo = aChannel->LoadInfo();

  nsCOMPtr<nsIFileChannel> fileChan(do_QueryInterface(aChannel));
  wasFileChannel = fileChan != nullptr;

  nsCOMPtr<nsIURI> referrer;
  NS_GetReferrerFromChannel(aChannel, getter_AddRefs(referrer));

  mozilla::net::LoadInfoArgs loadInfoArgs;
  MOZ_ALWAYS_SUCCEEDS(LoadInfoToLoadInfoArgs(loadInfo, &loadInfoArgs));

  // Now we build a protocol for forwarding our data to the parent.  The
  // protocol will act as a listener on the child-side and create a "real"
  // helperAppService listener on the parent-side, via another call to
  // DoContent.
  RefPtr<ExternalHelperAppChild> childListener = new ExternalHelperAppChild();
  MOZ_ALWAYS_TRUE(child->SendPExternalHelperAppConstructor(
      childListener, uri, loadInfoArgs, nsCString(aMimeContentType), disp,
      contentDisposition, fileName, aForceSave, contentLength, wasFileChannel,
      referrer, aContentContext));

  NS_ADDREF(*aStreamListener = childListener);

  uint32_t reason = nsIHelperAppLauncherDialog::REASON_CANTHANDLE;

  SanitizeFileName(fileName, 0);

  RefPtr<nsExternalAppHandler> handler =
      new nsExternalAppHandler(nullptr, u""_ns, aContentContext, aWindowContext,
                               this, fileName, reason, aForceSave);
  if (!handler) {
    return NS_ERROR_OUT_OF_MEMORY;
  }

  childListener->SetHandler(handler);
  return NS_OK;
}

NS_IMETHODIMP nsExternalHelperAppService::CreateListener(
    const nsACString& aMimeContentType, nsIChannel* aChannel,
    BrowsingContext* aContentContext, bool aForceSave,
    nsIInterfaceRequestor* aWindowContext,
    nsIStreamListener** aStreamListener) {
  MOZ_ASSERT(!XRE_IsContentProcess());
  NS_ENSURE_ARG_POINTER(aChannel);

  nsAutoString fileName;
  nsAutoCString fileExtension;
  uint32_t reason = nsIHelperAppLauncherDialog::REASON_CANTHANDLE;

  uint32_t contentDisposition = -1;
  aChannel->GetContentDisposition(&contentDisposition);
  if (contentDisposition == nsIChannel::DISPOSITION_ATTACHMENT) {
    reason = nsIHelperAppLauncherDialog::REASON_SERVERREQUEST;
  }

  *aStreamListener = nullptr;

  // Get the file extension and name that we will need later
  nsCOMPtr<nsIURI> uri;
  bool allowURLExtension =
      GetFileNameFromChannel(aChannel, fileName, getter_AddRefs(uri));

  uint32_t flags = VALIDATE_ALLOW_EMPTY;
  if (aMimeContentType.Equals(APPLICATION_GUESS_FROM_EXT,
                              nsCaseInsensitiveCStringComparator)) {
    flags |= VALIDATE_GUESS_FROM_EXTENSION;
  }

  nsCOMPtr<nsIMIMEInfo> mimeInfo = ValidateFileNameForSaving(
      fileName, aMimeContentType, uri, nullptr, flags, allowURLExtension);

  LOG("Type/Ext lookup found 0x%p\n", mimeInfo.get());

  // No mimeinfo -> we can't continue. probably OOM.
  if (!mimeInfo) {
    return NS_ERROR_OUT_OF_MEMORY;
  }

  if (flags & VALIDATE_GUESS_FROM_EXTENSION) {
    // Replace the content type with what was guessed.
    nsAutoCString mimeType;
    mimeInfo->GetMIMEType(mimeType);
    aChannel->SetContentType(mimeType);

    if (reason == nsIHelperAppLauncherDialog::REASON_CANTHANDLE) {
      reason = nsIHelperAppLauncherDialog::REASON_TYPESNIFFED;
    }
  }

  nsAutoString extension;
  int32_t dotidx = fileName.RFind(u".");
  if (dotidx != -1) {
    extension = Substring(fileName, dotidx + 1);
  }

  // NB: ExternalHelperAppParent depends on this listener always being an
  // nsExternalAppHandler. If this changes, make sure to update that code.
  nsExternalAppHandler* handler = new nsExternalAppHandler(
      mimeInfo, extension, aContentContext, aWindowContext, this, fileName,
      reason, aForceSave);
  if (!handler) {
    return NS_ERROR_OUT_OF_MEMORY;
  }

  NS_ADDREF(*aStreamListener = handler);
  return NS_OK;
}

NS_IMETHODIMP nsExternalHelperAppService::DoContent(
    const nsACString& aMimeContentType, nsIChannel* aChannel,
    nsIInterfaceRequestor* aContentContext, bool aForceSave,
    nsIInterfaceRequestor* aWindowContext,
    nsIStreamListener** aStreamListener) {
  // Scripted interface requestors cannot return an instance of the
  // (non-scriptable) nsPIDOMWindowOuter or nsPIDOMWindowInner interfaces, so
  // get to the window via `nsIDOMWindow`.  Unfortunately, at that point we
  // don't know whether the thing we got is an inner or outer window, so have to
  // work with either one.
  RefPtr<BrowsingContext> bc;
  nsCOMPtr<nsIDOMWindow> domWindow = do_GetInterface(aContentContext);
  if (nsCOMPtr<nsPIDOMWindowOuter> outerWindow = do_QueryInterface(domWindow)) {
    bc = outerWindow->GetBrowsingContext();
  } else if (nsCOMPtr<nsPIDOMWindowInner> innerWindow =
                 do_QueryInterface(domWindow)) {
    bc = innerWindow->GetBrowsingContext();
  }

  if (XRE_IsContentProcess()) {
    return DoContentContentProcessHelper(aMimeContentType, aChannel, bc,
                                         aForceSave, aWindowContext,
                                         aStreamListener);
  }

  nsresult rv = CreateListener(aMimeContentType, aChannel, bc, aForceSave,
                               aWindowContext, aStreamListener);
  return rv;
}

NS_IMETHODIMP nsExternalHelperAppService::ApplyDecodingForExtension(
    const nsACString& aExtension, const nsACString& aEncodingType,
    bool* aApplyDecoding) {
  *aApplyDecoding = true;
  uint32_t i;
  for (i = 0; i < std::size(nonDecodableExtensions); ++i) {
    if (aExtension.LowerCaseEqualsASCII(
            nonDecodableExtensions[i].mFileExtension) &&
        aEncodingType.LowerCaseEqualsASCII(
            nonDecodableExtensions[i].mMimeType)) {
      *aApplyDecoding = false;
      break;
    }
  }
  return NS_OK;
}

nsresult nsExternalHelperAppService::GetFileTokenForPath(
    const char16_t* aPlatformAppPath, nsIFile** aFile) {
  nsDependentString platformAppPath(aPlatformAppPath);
  // First, check if we have an absolute path
  nsIFile* localFile = nullptr;
  nsresult rv = NS_NewLocalFile(platformAppPath, &localFile);
  if (NS_SUCCEEDED(rv)) {
    *aFile = localFile;
    bool exists;
    if (NS_FAILED((*aFile)->Exists(&exists)) || !exists) {
      NS_RELEASE(*aFile);
      return NS_ERROR_FILE_NOT_FOUND;
    }
    return NS_OK;
  }

  // Second, check if file exists in mozilla program directory
  rv = NS_GetSpecialDirectory(NS_XPCOM_CURRENT_PROCESS_DIR, aFile);
  if (NS_SUCCEEDED(rv)) {
    rv = (*aFile)->Append(platformAppPath);
    if (NS_SUCCEEDED(rv)) {
      bool exists = false;
      rv = (*aFile)->Exists(&exists);
      if (NS_SUCCEEDED(rv) && exists) return NS_OK;
    }
    NS_RELEASE(*aFile);
  }

  return NS_ERROR_NOT_AVAILABLE;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////
// begin external protocol service default implementation...
//////////////////////////////////////////////////////////////////////////////////////////////////////
NS_IMETHODIMP nsExternalHelperAppService::ExternalProtocolHandlerExists(
    const char* aProtocolScheme, bool* aHandlerExists) {
  nsCOMPtr<nsIHandlerInfo> handlerInfo;
  nsresult rv = GetProtocolHandlerInfo(nsDependentCString(aProtocolScheme),
                                       getter_AddRefs(handlerInfo));
  if (NS_SUCCEEDED(rv)) {
    // See if we have any known possible handler apps for this
    nsCOMPtr<nsIMutableArray> possibleHandlers;
    handlerInfo->GetPossibleApplicationHandlers(
        getter_AddRefs(possibleHandlers));

    uint32_t length;
    possibleHandlers->GetLength(&length);
    if (length) {
      *aHandlerExists = true;
      return NS_OK;
    }
  }

  // if not, fall back on an os-based handler
  return OSProtocolHandlerExists(aProtocolScheme, aHandlerExists);
}

NS_IMETHODIMP nsExternalHelperAppService::IsExposedProtocol(
    const char* aProtocolScheme, bool* aResult) {
  // check the per protocol setting first.  it always takes precedence.
  // if not set, then use the global setting.

  nsAutoCString prefName("network.protocol-handler.expose.");
  prefName += aProtocolScheme;
  bool val;
  if (NS_SUCCEEDED(Preferences::GetBool(prefName.get(), &val))) {
    *aResult = val;
    return NS_OK;
  }

  // by default, no protocol is exposed.  i.e., by default all link clicks must
  // go through the external protocol service.  most applications override this
  // default behavior.
  *aResult = Preferences::GetBool("network.protocol-handler.expose-all", false);

  return NS_OK;
}

static const char kExternalProtocolPrefPrefix[] =
    "network.protocol-handler.external.";
static const char kExternalProtocolDefaultPref[] =
    "network.protocol-handler.external-default";

// static
nsresult nsExternalHelperAppService::EscapeURI(nsIURI* aURI, nsIURI** aResult) {
  MOZ_ASSERT(aURI);
  MOZ_ASSERT(aResult);

  nsAutoCString spec;
  aURI->GetSpec(spec);

  if (spec.Find("%00") != -1) return NS_ERROR_MALFORMED_URI;

  nsAutoCString escapedSpec;
  nsresult rv = NS_EscapeURL(spec, esc_AlwaysCopy | esc_ExtHandler, escapedSpec,
                             fallible);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIIOService> ios(do_GetIOService());
  return ios->NewURI(escapedSpec, nullptr, nullptr, aResult);
}

bool ExternalProtocolIsBlockedBySandbox(
    BrowsingContext* aBrowsingContext,
    const bool aHasValidUserGestureActivation) {
  if (!StaticPrefs::dom_block_external_protocol_navigation_from_sandbox()) {
    return false;
  }

  if (!aBrowsingContext || aBrowsingContext->IsTop()) {
    return false;
  }

  uint32_t sandboxFlags = aBrowsingContext->GetSandboxFlags();

  if (sandboxFlags == SANDBOXED_NONE) {
    return false;
  }

  if (!(sandboxFlags & SANDBOXED_AUXILIARY_NAVIGATION)) {
    return false;
  }

  if (!(sandboxFlags & SANDBOXED_TOPLEVEL_NAVIGATION)) {
    return false;
  }

  if (!(sandboxFlags & SANDBOXED_TOPLEVEL_NAVIGATION_CUSTOM_PROTOCOLS)) {
    return false;
  }

  if (!(sandboxFlags & SANDBOXED_TOPLEVEL_NAVIGATION_USER_ACTIVATION) &&
      aHasValidUserGestureActivation) {
    return false;
  }

  return true;
}

NS_IMETHODIMP
nsExternalHelperAppService::LoadURI(nsIURI* aURI,
                                    nsIPrincipal* aTriggeringPrincipal,
                                    nsIPrincipal* aRedirectPrincipal,
                                    BrowsingContext* aBrowsingContext,
                                    bool aTriggeredExternally,
                                    bool aHasValidUserGestureActivation,
                                    bool aNewWindowTarget) {
  NS_ENSURE_ARG_POINTER(aURI);

  if (XRE_IsContentProcess()) {
    mozilla::dom::ContentChild::GetSingleton()->SendLoadURIExternal(
        aURI, aTriggeringPrincipal, aRedirectPrincipal, aBrowsingContext,
        aTriggeredExternally, aHasValidUserGestureActivation, aNewWindowTarget);
    return NS_OK;
  }

  // Prevent sandboxed BrowsingContexts from navigating to external protocols.
  // This only uses the sandbox flags of the target BrowsingContext of the
  // load. The navigating document's CSP sandbox flags do not apply.
  if (aBrowsingContext &&
      ExternalProtocolIsBlockedBySandbox(aBrowsingContext,
                                         aHasValidUserGestureActivation)) {
    // Log an error to the web console of the sandboxed BrowsingContext.
    nsAutoString localizedMsg;
    nsAutoCString spec;
    aURI->GetSpec(spec);

    AutoTArray<nsString, 1> params = {NS_ConvertUTF8toUTF16(spec)};
    nsresult rv = nsContentUtils::FormatLocalizedString(
        nsContentUtils::eSECURITY_PROPERTIES, "SandboxBlockedCustomProtocols",
        params, localizedMsg);
    NS_ENSURE_SUCCESS(rv, rv);

    // Log to the the parent window of the iframe. If there is no parent, fall
    // back to the iframe window itself.
    WindowContext* windowContext = aBrowsingContext->GetParentWindowContext();
    if (!windowContext) {
      windowContext = aBrowsingContext->GetCurrentWindowContext();
    }

    // Skip logging if we still don't have a WindowContext.
    NS_ENSURE_TRUE(windowContext, NS_ERROR_FAILURE);

    nsContentUtils::ReportToConsoleByWindowID(
        localizedMsg, nsIScriptError::errorFlag, "Security"_ns,
        windowContext->InnerWindowId(),
        SourceLocation(windowContext->Canonical()->GetDocumentURI()));

    return NS_OK;
  }

  nsCOMPtr<nsIURI> escapedURI;
  nsresult rv = EscapeURI(aURI, getter_AddRefs(escapedURI));
  NS_ENSURE_SUCCESS(rv, rv);

  nsAutoCString scheme;
  escapedURI->GetScheme(scheme);
  if (scheme.IsEmpty()) return NS_OK;  // must have a scheme

  // Deny load if the prefs say to do so
  nsAutoCString externalPref(kExternalProtocolPrefPrefix);
  externalPref += scheme;
  bool allowLoad = false;
  if (NS_FAILED(Preferences::GetBool(externalPref.get(), &allowLoad))) {
    // no scheme-specific value, check the default
    if (NS_FAILED(
            Preferences::GetBool(kExternalProtocolDefaultPref, &allowLoad))) {
      return NS_OK;  // missing default pref
    }
  }

  if (!allowLoad) {
    return NS_OK;  // explicitly denied
  }

  // Now check if the principal is allowed to access the navigated context.
  // We allow navigating subframes, even if not same-origin - non-external
  // links can always navigate everywhere, so this is a minor additional
  // restriction, only aiming to prevent some types of spoofing attacks
  // from otherwise disjoint browsingcontext trees.
  if (aBrowsingContext && aTriggeringPrincipal &&
      // Add-on principals are always allowed:
      !BasePrincipal::Cast(aTriggeringPrincipal)->AddonPolicy() &&
      // As is chrome code:
      !aTriggeringPrincipal->IsSystemPrincipal()) {
    RefPtr<BrowsingContext> bc = aBrowsingContext;
    WindowGlobalParent* wgp = bc->Canonical()->GetCurrentWindowGlobal();
    bool foundAccessibleFrame = false;

    // Don't block the load if it is the first load in a new window (e.g. due to
    // a call to window.open, or a target=_blank link click).
    if (aNewWindowTarget) {
      MOZ_ASSERT(bc->IsTop());
      foundAccessibleFrame = true;
    }

    // Also allow this load if the target is a toplevel BC which contains a
    // non-web-controlled about:blank document.
    // NOTE: This catches cases like shift-clicking a link which do not set
    // `newWindowTarget`, but do open a link in a new window on behalf of web
    // content.
    if (!foundAccessibleFrame && bc->IsTop() &&
        !bc->GetTopLevelCreatedByWebContent() && wgp) {
      nsIURI* uri = wgp->GetDocumentURI();
      foundAccessibleFrame = uri && NS_IsAboutBlank(uri);
    }

    while (!foundAccessibleFrame) {
      if (wgp) {
        foundAccessibleFrame =
            aTriggeringPrincipal->Subsumes(wgp->DocumentPrincipal());
      }
      // We have to get the parent via the bc, because there may not
      // be a window global for the innermost bc; see bug 1650162.
      BrowsingContext* parent = bc->GetParent();
      if (!parent) {
        break;
      }
      bc = parent;
      wgp = parent->Canonical()->GetCurrentWindowGlobal();
    }

    if (!foundAccessibleFrame) {
      // See if this navigation could have come from a subframe.
      nsTArray<RefPtr<BrowsingContext>> contexts;
      aBrowsingContext->GetAllBrowsingContextsInSubtree(contexts);
      for (const auto& kid : contexts) {
        wgp = kid->Canonical()->GetCurrentWindowGlobal();
        if (wgp && aTriggeringPrincipal->Subsumes(wgp->DocumentPrincipal())) {
          foundAccessibleFrame = true;
          break;
        }
      }
    }

    if (!foundAccessibleFrame) {
      return NS_OK;  // deny the load.
    }
  }

  nsCOMPtr<nsIHandlerInfo> handler;
  rv = GetProtocolHandlerInfo(scheme, getter_AddRefs(handler));
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIContentDispatchChooser> chooser =
      do_CreateInstance("@mozilla.org/content-dispatch-chooser;1", &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  return chooser->HandleURI(
      handler, escapedURI,
      aRedirectPrincipal ? aRedirectPrincipal : aTriggeringPrincipal,
      aBrowsingContext, aTriggeredExternally);
}

//////////////////////////////////////////////////////////////////////////////////////////////////////
// Methods related to deleting temporary files on exit
//////////////////////////////////////////////////////////////////////////////////////////////////////

/* static */
nsresult nsExternalHelperAppService::DeleteTemporaryFileHelper(
    nsIFile* aTemporaryFile, nsCOMArray<nsIFile>& aFileList) {
  bool isFile = false;

  // as a safety measure, make sure the nsIFile is really a file and not a
  // directory object.
  aTemporaryFile->IsFile(&isFile);
  if (!isFile) return NS_OK;

  aFileList.AppendObject(aTemporaryFile);

  return NS_OK;
}

NS_IMETHODIMP
nsExternalHelperAppService::DeleteTemporaryFileOnExit(nsIFile* aTemporaryFile) {
  return DeleteTemporaryFileHelper(aTemporaryFile, mTemporaryFilesList);
}

NS_IMETHODIMP
nsExternalHelperAppService::DeleteTemporaryPrivateFileWhenPossible(
    nsIFile* aTemporaryFile) {
  return DeleteTemporaryFileHelper(aTemporaryFile, mTemporaryPrivateFilesList);
}

void nsExternalHelperAppService::ExpungeTemporaryFilesHelper(
    nsCOMArray<nsIFile>& fileList) {
  int32_t numEntries = fileList.Count();
  nsIFile* localFile;
  for (int32_t index = 0; index < numEntries; index++) {
    localFile = fileList[index];
    if (localFile) {
      // First make the file writable, since the temp file is probably readonly.
      localFile->SetPermissions(0600);
      localFile->Remove(false);
    }
  }

  fileList.Clear();
}

void nsExternalHelperAppService::ExpungeTemporaryFiles() {
  ExpungeTemporaryFilesHelper(mTemporaryFilesList);
}

void nsExternalHelperAppService::ExpungeTemporaryPrivateFiles() {
  ExpungeTemporaryFilesHelper(mTemporaryPrivateFilesList);
}

static const char kExternalWarningPrefPrefix[] =
    "network.protocol-handler.warn-external.";
static const char kExternalWarningDefaultPref[] =
    "network.protocol-handler.warn-external-default";

NS_IMETHODIMP
nsExternalHelperAppService::GetProtocolHandlerInfo(
    const nsACString& aScheme, nsIHandlerInfo** aHandlerInfo) {
  // XXX enterprise customers should be able to turn this support off with a
  // single master pref (maybe use one of the "exposed" prefs here?)

  bool exists;
  nsresult rv = GetProtocolHandlerInfoFromOS(aScheme, &exists, aHandlerInfo);
  if (NS_FAILED(rv)) {
    // Either it knows nothing, or we ran out of memory
    return NS_ERROR_FAILURE;
  }

  nsCOMPtr<nsIHandlerService> handlerSvc =
      do_GetService(NS_HANDLERSERVICE_CONTRACTID);
  if (handlerSvc) {
    bool hasHandler = false;
    (void)handlerSvc->Exists(*aHandlerInfo, &hasHandler);
    if (hasHandler) {
      rv = handlerSvc->FillHandlerInfo(*aHandlerInfo, ""_ns);
      if (NS_SUCCEEDED(rv)) return NS_OK;
    }
  }

  return SetProtocolHandlerDefaults(*aHandlerInfo, exists);
}

NS_IMETHODIMP
nsExternalHelperAppService::SetProtocolHandlerDefaults(
    nsIHandlerInfo* aHandlerInfo, bool aOSHandlerExists) {
  // this type isn't in our database, so we've only got an OS default handler,
  // if one exists

  if (aOSHandlerExists) {
    // we've got a default, so use it
    aHandlerInfo->SetPreferredAction(nsIHandlerInfo::useSystemDefault);

    // whether or not to ask the user depends on the warning preference
    nsAutoCString scheme;
    aHandlerInfo->GetType(scheme);

    nsAutoCString warningPref(kExternalWarningPrefPrefix);
    warningPref += scheme;
    bool warn;
    if (NS_FAILED(Preferences::GetBool(warningPref.get(), &warn))) {
      // no scheme-specific value, check the default
      warn = Preferences::GetBool(kExternalWarningDefaultPref, true);
    }
    aHandlerInfo->SetAlwaysAskBeforeHandling(warn);
  } else {
    // If no OS default existed, we set the preferred action to alwaysAsk.
    // This really means not initialized (i.e. there's no available handler)
    // to all the code...
    aHandlerInfo->SetPreferredAction(nsIHandlerInfo::alwaysAsk);
  }

  return NS_OK;
}

// XPCOM profile change observer
NS_IMETHODIMP
nsExternalHelperAppService::Observe(nsISupports* aSubject, const char* aTopic,
                                    const char16_t* someData) {
  if (!strcmp(aTopic, "profile-before-change")) {
    ExpungeTemporaryFiles();
  } else if (!strcmp(aTopic, "last-pb-context-exited")) {
    ExpungeTemporaryPrivateFiles();
  }
  return NS_OK;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////
// begin external app handler implementation
//////////////////////////////////////////////////////////////////////////////////////////////////////

NS_IMPL_ADDREF(nsExternalAppHandler)
NS_IMPL_RELEASE(nsExternalAppHandler)

NS_INTERFACE_MAP_BEGIN(nsExternalAppHandler)
  NS_INTERFACE_MAP_ENTRY_AMBIGUOUS(nsISupports, nsIStreamListener)
  NS_INTERFACE_MAP_ENTRY(nsIStreamListener)
  NS_INTERFACE_MAP_ENTRY(nsIRequestObserver)
  NS_INTERFACE_MAP_ENTRY(nsIHelperAppLauncher)
  NS_INTERFACE_MAP_ENTRY(nsICancelable)
  NS_INTERFACE_MAP_ENTRY(nsIBackgroundFileSaverObserver)
  NS_INTERFACE_MAP_ENTRY(nsINamed)
  NS_INTERFACE_MAP_ENTRY_CONCRETE(nsExternalAppHandler)
NS_INTERFACE_MAP_END

nsExternalAppHandler::nsExternalAppHandler(
    nsIMIMEInfo* aMIMEInfo, const nsAString& aFileExtension,
    BrowsingContext* aBrowsingContext, nsIInterfaceRequestor* aWindowContext,
    nsExternalHelperAppService* aExtProtSvc,
    const nsAString& aSuggestedFileName, uint32_t aReason, bool aForceSave)
    : mMimeInfo(aMIMEInfo),
      mBrowsingContext(aBrowsingContext),
      mWindowContext(aWindowContext),
      mSuggestedFileName(aSuggestedFileName),
      mForceSave(aForceSave),
      mForceSaveInternallyHandled(false),
      mCanceled(false),
      mStopRequestIssued(false),
      mIsFileChannel(false),
      mHandleInternally(false),
      mDialogShowing(false),
      mReason(aReason),
      mTempFileIsExecutable(false),
      mTimeDownloadStarted(0),
      mContentLength(-1),
      mProgress(0),
      mSaver(nullptr),
      mDialogProgressListener(nullptr),
      mTransfer(nullptr),
      mRequest(nullptr),
      mExtProtSvc(aExtProtSvc) {
  // make sure the extention includes the '.'
  if (!aFileExtension.IsEmpty() && aFileExtension.First() != '.') {
    mFileExtension = char16_t('.');
  }
  mFileExtension.Append(aFileExtension);

  mBufferSize = Preferences::GetUint("network.buffer.cache.size", 4096);
}

nsExternalAppHandler::~nsExternalAppHandler() {
  MOZ_ASSERT(!mSaver, "Saver should hold a reference to us until deleted");
}

void nsExternalAppHandler::DidDivertRequest(nsIRequest* request) {
  MOZ_ASSERT(XRE_IsContentProcess(), "in child process");
  // Remove our request from the child loadGroup
  RetargetLoadNotifications(request);
}

NS_IMETHODIMP nsExternalAppHandler::SetWebProgressListener(
    nsIWebProgressListener2* aWebProgressListener) {
  // This is always called by nsHelperDlg.js. Go ahead and register the
  // progress listener. At this point, we don't have mTransfer.
  mDialogProgressListener = aWebProgressListener;
  return NS_OK;
}

NS_IMETHODIMP nsExternalAppHandler::GetTargetFile(nsIFile** aTarget) {
  if (mFinalFileDestination)
    *aTarget = mFinalFileDestination;
  else
    *aTarget = mTempFile;

  NS_IF_ADDREF(*aTarget);
  return NS_OK;
}

NS_IMETHODIMP nsExternalAppHandler::GetTargetFileIsExecutable(bool* aExec) {
  // Use the real target if it's been set
  if (mFinalFileDestination) return mFinalFileDestination->IsExecutable(aExec);

  // Otherwise, use the stored executable-ness of the temporary
  *aExec = mTempFileIsExecutable;
  return NS_OK;
}

NS_IMETHODIMP nsExternalAppHandler::GetTimeDownloadStarted(PRTime* aTime) {
  *aTime = mTimeDownloadStarted;
  return NS_OK;
}

NS_IMETHODIMP nsExternalAppHandler::GetContentLength(int64_t* aContentLength) {
  *aContentLength = mContentLength;
  return NS_OK;
}

NS_IMETHODIMP nsExternalAppHandler::GetBrowsingContextId(
    uint64_t* aBrowsingContextId) {
  *aBrowsingContextId = mBrowsingContext ? mBrowsingContext->Id() : 0;
  return NS_OK;
}

void nsExternalAppHandler::RetargetLoadNotifications(nsIRequest* request) {
  // we are going to run the downloading of the helper app in our own little
  // docloader / load group context. so go ahead and force the creation of a
  // load group and doc loader for us to use...
  nsCOMPtr<nsIChannel> aChannel = do_QueryInterface(request);
  if (!aChannel) return;

  bool isPrivate = NS_UsePrivateBrowsing(aChannel);

  nsCOMPtr<nsILoadGroup> oldLoadGroup;
  aChannel->GetLoadGroup(getter_AddRefs(oldLoadGroup));

  if (oldLoadGroup) {
    oldLoadGroup->RemoveRequest(request, nullptr, NS_BINDING_RETARGETED);
  }

  aChannel->SetLoadGroup(nullptr);
  aChannel->SetNotificationCallbacks(nullptr);

  nsCOMPtr<nsIPrivateBrowsingChannel> pbChannel = do_QueryInterface(aChannel);
  if (pbChannel) {
    pbChannel->SetPrivate(isPrivate);
  }
}

nsresult nsExternalAppHandler::SetUpTempFile(nsIChannel* aChannel) {
  // First we need to try to get the destination directory for the temporary
  // file.
  auto res = GetInitialDownloadDirectory();
  if (res.isErr()) return res.unwrapErr();
  mTempFile = res.unwrap();

  // At this point, we do not have a filename for the temp file.  For security
  // purposes, this cannot be predictable, so we must use a cryptographic
  // quality PRNG to generate one.
  nsAutoCString tempLeafName;
  nsresult rv = GenerateRandomName(tempLeafName);
  NS_ENSURE_SUCCESS(rv, rv);

  // now append our extension.
  nsAutoCString ext;
  mMimeInfo->GetPrimaryExtension(ext);
  if (!ext.IsEmpty()) {
    ext.ReplaceChar(KNOWN_PATH_SEPARATORS FILE_ILLEGAL_CHARACTERS, '_');
    if (ext.First() != '.') tempLeafName.Append('.');
    tempLeafName.Append(ext);
  }

  // We need to temporarily create a dummy file with the correct
  // file extension to determine the executable-ness, so do this before adding
  // the extra .part extension.
  nsCOMPtr<nsIFile> dummyFile;
  rv = mTempFile->Clone(getter_AddRefs(dummyFile));
  NS_ENSURE_SUCCESS(rv, rv);

  // Set the file name without .part
  rv = dummyFile->Append(NS_ConvertUTF8toUTF16(tempLeafName));
  NS_ENSURE_SUCCESS(rv, rv);
  rv = dummyFile->CreateUnique(nsIFile::NORMAL_FILE_TYPE, 0600);
  NS_ENSURE_SUCCESS(rv, rv);

  // Store executable-ness then delete
  dummyFile->IsExecutable(&mTempFileIsExecutable);
  dummyFile->Remove(false);

  // Add an additional .part to prevent the OS from running this file in the
  // default application.
  tempLeafName.AppendLiteral(".part");

  rv = mTempFile->Append(NS_ConvertUTF8toUTF16(tempLeafName));
  // make this file unique!!!
  NS_ENSURE_SUCCESS(rv, rv);
  rv = mTempFile->CreateUnique(nsIFile::NORMAL_FILE_TYPE, 0600);
  NS_ENSURE_SUCCESS(rv, rv);

  // Now save the temp leaf name, minus the ".part" bit, so we can use it later.
  // This is a bit broken in the case when createUnique actually had to append
  // some numbers, because then we now have a filename like foo.bar-1.part and
  // we'll end up with foo.bar-1.bar as our final filename if we end up using
  // this.  But the other options are all bad too....  Ideally we'd have a way
  // to tell createUnique to put its unique marker before the extension that
  // comes before ".part" or something.
  rv = mTempFile->GetLeafName(mTempLeafName);
  NS_ENSURE_SUCCESS(rv, rv);

  NS_ENSURE_TRUE(StringEndsWith(mTempLeafName, u".part"_ns),
                 NS_ERROR_UNEXPECTED);

  // Strip off the ".part" from mTempLeafName
  mTempLeafName.Truncate(mTempLeafName.Length() - std::size(".part") + 1);

  MOZ_ASSERT(!mSaver, "Output file initialization called more than once!");
  mSaver =
      do_CreateInstance(NS_BACKGROUNDFILESAVERSTREAMLISTENER_CONTRACTID, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = mSaver->SetObserver(this);
  if (NS_FAILED(rv)) {
    mSaver = nullptr;
    return rv;
  }

  rv = mSaver->EnableSha256();
  NS_ENSURE_SUCCESS(rv, rv);

  rv = mSaver->EnableSignatureInfo();
  NS_ENSURE_SUCCESS(rv, rv);
  LOG("Enabled hashing and signature verification");

  rv = mSaver->SetTarget(mTempFile, false);
  NS_ENSURE_SUCCESS(rv, rv);

  return rv;
}

void nsExternalAppHandler::MaybeApplyDecodingForExtension(
    nsIRequest* aRequest) {
  MOZ_ASSERT(aRequest);

  nsCOMPtr<nsIEncodedChannel> encChannel = do_QueryInterface(aRequest);
  if (!encChannel) {
    return;
  }

  // Turn off content encoding conversions if needed
  bool applyConversion = true;

  // First, check to see if conversion is already disabled.  If so, we
  // have nothing to do here.
  encChannel->GetApplyConversion(&applyConversion);
  if (!applyConversion) {
    return;
  }

  nsCOMPtr<nsIURL> sourceURL(do_QueryInterface(mSourceUrl));
  if (sourceURL) {
    nsAutoCString extension;
    sourceURL->GetFileExtension(extension);
    if (!extension.IsEmpty()) {
      nsCOMPtr<nsIUTF8StringEnumerator> encEnum;
      encChannel->GetContentEncodings(getter_AddRefs(encEnum));
      if (encEnum) {
        bool hasMore;
        nsresult rv = encEnum->HasMore(&hasMore);
        if (NS_SUCCEEDED(rv) && hasMore) {
          nsAutoCString encType;
          rv = encEnum->GetNext(encType);
          if (NS_SUCCEEDED(rv) && !encType.IsEmpty()) {
            MOZ_ASSERT(mExtProtSvc);
            mExtProtSvc->ApplyDecodingForExtension(extension, encType,
                                                   &applyConversion);
          }
        }
      }
    }
  }

  encChannel->SetApplyConversion(applyConversion);
}

already_AddRefed<nsIInterfaceRequestor>
nsExternalAppHandler::GetDialogParent() {
  nsCOMPtr<nsIInterfaceRequestor> dialogParent = mWindowContext;

  if (!dialogParent && mBrowsingContext) {
    dialogParent = do_QueryInterface(mBrowsingContext->GetDOMWindow());
  }
  if (!dialogParent && mBrowsingContext && XRE_IsParentProcess()) {
    RefPtr<Element> element = mBrowsingContext->Top()->GetEmbedderElement();
    if (element) {
      dialogParent = do_QueryInterface(element->OwnerDoc()->GetWindow());
    }
  }
  return dialogParent.forget();
}

NS_IMETHODIMP nsExternalAppHandler::OnStartRequest(nsIRequest* request) {
  MOZ_ASSERT(request, "OnStartRequest without request?");

  // Set mTimeDownloadStarted here as the download has already started and
  // we want to record the start time before showing the filepicker.
  mTimeDownloadStarted = PR_Now();

  mRequest = request;

  nsCOMPtr<nsIChannel> aChannel = do_QueryInterface(request);

  nsresult rv;
  nsAutoCString MIMEType;
  if (mMimeInfo) {
    mMimeInfo->GetMIMEType(MIMEType);
  }
  // Now get the URI
  if (aChannel) {
    aChannel->GetURI(getter_AddRefs(mSourceUrl));
    // HTTPS-Only/HTTPS-FirstMode tries to upgrade connections to https. Once
    // the download is in progress we set that flag so that timeout counter
    // measures do not kick in.
    nsCOMPtr<nsILoadInfo> loadInfo = aChannel->LoadInfo();
    if (nsHTTPSOnlyUtils::GetUpgradeMode(loadInfo) !=
        nsHTTPSOnlyUtils::NO_UPGRADE_MODE) {
      uint32_t httpsOnlyStatus = loadInfo->GetHttpsOnlyStatus();
      httpsOnlyStatus |= nsILoadInfo::HTTPS_ONLY_DOWNLOAD_IN_PROGRESS;
      loadInfo->SetHttpsOnlyStatus(httpsOnlyStatus);
    }
  }

  if (!mForceSave && StaticPrefs::browser_download_enable_spam_prevention() &&
      IsDownloadSpam(aChannel)) {
    return NS_OK;
  }

  mDownloadClassification =
      nsContentSecurityUtils::ClassifyDownload(aChannel, MIMEType);

  if (mDownloadClassification == nsITransfer::DOWNLOAD_FORBIDDEN) {
    // If the download is rated as forbidden,
    // cancel the request so no ui knows about this.
    mCanceled = true;
    request->Cancel(NS_ERROR_ABORT);
    return NS_OK;
  }

  nsCOMPtr<nsIFileChannel> fileChan(do_QueryInterface(request));
  mIsFileChannel = fileChan != nullptr;
  if (!mIsFileChannel) {
    // It's possible that this request came from the child process and the
    // file channel actually lives there. If this returns true, then our
    // mSourceUrl will be an nsIFileURL anyway.
    nsCOMPtr<dom::nsIExternalHelperAppParent> parent(
        do_QueryInterface(request));
    mIsFileChannel = parent && parent->WasFileChannel();
  }

  // Get content length
  if (aChannel) {
    aChannel->GetContentLength(&mContentLength);
  }

  if (mBrowsingContext) {
    mMaybeCloseWindowHelper = new MaybeCloseWindowHelper(mBrowsingContext);

    // Determine whether a new window was opened specifically for this request
    if (aChannel) {
      nsCOMPtr<nsILoadInfo> loadInfo = aChannel->LoadInfo();
      mMaybeCloseWindowHelper->SetShouldCloseWindow(
          loadInfo->GetIsNewWindowTarget());
    }
  }

  // retarget all load notifications to our docloader instead of the original
  // window's docloader...
  RetargetLoadNotifications(request);

  // Close the underlying DOMWindow if it was opened specifically for the
  // download. We don't run this in the content process, since we have
  // an instance running in the parent as well, which will handle this
  // if needed.
  if (!XRE_IsContentProcess() && mMaybeCloseWindowHelper) {
    mBrowsingContext = mMaybeCloseWindowHelper->MaybeCloseWindow();
  }

  // In an IPC setting, we're allowing the child process, here, to make
  // decisions about decoding the channel (e.g. decompression).  It will
  // still forward the decoded (uncompressed) data back to the parent.
  // Con: Uncompressed data means more IPC overhead.
  // Pros: ExternalHelperAppParent doesn't need to implement nsIEncodedChannel.
  //       Parent process doesn't need to expect CPU time on decompression.
  MaybeApplyDecodingForExtension(aChannel);

  // At this point, the child process has done everything it can usefully do
  // for OnStartRequest.
  if (XRE_IsContentProcess()) {
    return NS_OK;
  }

  rv = SetUpTempFile(aChannel);
  if (NS_FAILED(rv)) {
    nsresult transferError = rv;

    rv = CreateFailedTransfer();
    if (NS_FAILED(rv)) {
      LOG("Failed to create transfer to report failure."
          "Will fallback to prompter!");
    }

    mCanceled = true;
    request->Cancel(transferError);

    auto res = GetInitialDownloadDirectory(true);
    if (res.isErr()) {
      // Just send the file name as we can't get a download path.
      // TODO: evaluate adding a more specific error here.
      SendStatusChange(kWriteError, transferError, request, mSuggestedFileName);
      return res.unwrapErr();
    }

    nsCOMPtr<nsIFile> pseudoFile = res.unwrap();
    MOZ_ALWAYS_SUCCEEDS(pseudoFile->Append(mSuggestedFileName));
    nsAutoString path;
    MOZ_ALWAYS_SUCCEEDS(pseudoFile->GetPath(path));
    SendStatusChange(kWriteError, transferError, request, path);

    return NS_OK;
  }

  // Inform channel it is open on behalf of a download to throttle it during
  // page loads and prevent its caching.
  nsCOMPtr<nsIHttpChannelInternal> httpInternal = do_QueryInterface(aChannel);
  if (httpInternal) {
    rv = httpInternal->SetChannelIsForDownload(true);
    MOZ_ASSERT(NS_SUCCEEDED(rv));
  }

  if (mSourceUrl->SchemeIs("data")) {
    // In case we're downloading a data:// uri
    // we don't want to apply AllowTopLevelNavigationToDataURI.
    nsCOMPtr<nsILoadInfo> loadInfo = aChannel->LoadInfo();
    loadInfo->SetForceAllowDataURI(true);
  }

  // now that the temp file is set up, find out if we need to invoke a dialog
  // asking the user what they want us to do with this content...

  // We can get here for three reasons: "can't handle", "sniffed type", or
  // "server sent content-disposition:attachment".  In the first case we want
  // to honor the user's "always ask" pref; in the other two cases we want to
  // honor it only if the default action is "save".  Opening attachments in
  // helper apps by default breaks some websites (especially if the attachment
  // is one part of a multipart document).  Opening sniffed content in helper
  // apps by default introduces security holes that we'd rather not have.

  // So let's find out whether the user wants to be prompted.  If he does not,
  // check mReason and the preferred action to see what we should do.

  bool alwaysAsk = true;
  mMimeInfo->GetAlwaysAskBeforeHandling(&alwaysAsk);
  if (alwaysAsk) {
    // But we *don't* ask if this mimeInfo didn't come from
    // our user configuration datastore and the user has said
    // at some point in the distant past that they don't
    // want to be asked.  The latter fact would have been
    // stored in pref strings back in the old days.

    bool mimeTypeIsInDatastore = false;
    nsCOMPtr<nsIHandlerService> handlerSvc =
        do_GetService(NS_HANDLERSERVICE_CONTRACTID);
    if (handlerSvc) {
      handlerSvc->Exists(mMimeInfo, &mimeTypeIsInDatastore);
    }
    if (!handlerSvc || !mimeTypeIsInDatastore) {
      if (!GetNeverAskFlagFromPref(NEVER_ASK_FOR_SAVE_TO_DISK_PREF,
                                   MIMEType.get())) {
        // Don't need to ask after all.
        alwaysAsk = false;
        // Make sure action matches pref (save to disk).
        mMimeInfo->SetPreferredAction(nsIMIMEInfo::saveToDisk);
      } else if (!GetNeverAskFlagFromPref(NEVER_ASK_FOR_OPEN_FILE_PREF,
                                          MIMEType.get())) {
        // Don't need to ask after all.
        alwaysAsk = false;
      }
    }
  } else if (MIMEType.EqualsLiteral("text/plain")) {
    nsAutoCString ext;
    mMimeInfo->GetPrimaryExtension(ext);
    // If people are sending us ApplicationReputation-eligible files with
    // text/plain mimetypes, enforce asking the user what to do.
    if (!ext.IsEmpty()) {
      nsAutoCString dummyFileName("f");
      if (ext.First() != '.') {
        dummyFileName.Append(".");
      }
      ext.ReplaceChar(KNOWN_PATH_SEPARATORS FILE_ILLEGAL_CHARACTERS, '_');
      nsCOMPtr<nsIApplicationReputationService> appRep =
          components::ApplicationReputation::Service();
      appRep->IsBinary(dummyFileName + ext, &alwaysAsk);
    }
  }

  int32_t action = nsIMIMEInfo::saveToDisk;
  mMimeInfo->GetPreferredAction(&action);

  bool forcePrompt = mReason == nsIHelperAppLauncherDialog::REASON_TYPESNIFFED;

  // OK, now check why we're here
  if (!alwaysAsk && forcePrompt) {
    // Force asking if we're not saving.  See comment back when we fetched the
    // alwaysAsk boolean for details.
    alwaysAsk = (action != nsIMIMEInfo::saveToDisk);
  }

  bool shouldAutomaticallyHandleInternally =
      action == nsIMIMEInfo::handleInternally;

  if (aChannel) {
    uint32_t disposition = -1;
    aChannel->GetContentDisposition(&disposition);
    mForceSaveInternallyHandled =
        shouldAutomaticallyHandleInternally &&
        disposition == nsIChannel::DISPOSITION_ATTACHMENT &&
        mozilla::StaticPrefs::
            browser_download_force_save_internally_handled_attachments();
  }

  // If we're not asking, check we actually know what to do:
  if (!alwaysAsk) {
    alwaysAsk = action != nsIMIMEInfo::saveToDisk &&
                action != nsIMIMEInfo::useHelperApp &&
                action != nsIMIMEInfo::useSystemDefault &&
                !shouldAutomaticallyHandleInternally;
  }

  // If we're handling with the OS default and we are that default, force
  // asking, so we don't end up in an infinite loop:
  if (!alwaysAsk && action == nsIMIMEInfo::useSystemDefault) {
    bool areOSDefault = false;
    alwaysAsk = NS_SUCCEEDED(mMimeInfo->IsCurrentAppOSDefault(&areOSDefault)) &&
                areOSDefault;
  } else if (!alwaysAsk && action == nsIMIMEInfo::useHelperApp) {
    nsCOMPtr<nsIHandlerApp> preferredApp;
    mMimeInfo->GetPreferredApplicationHandler(getter_AddRefs(preferredApp));
    nsCOMPtr<nsILocalHandlerApp> handlerApp = do_QueryInterface(preferredApp);
    if (handlerApp) {
      nsCOMPtr<nsIFile> executable;
      handlerApp->GetExecutable(getter_AddRefs(executable));
      nsCOMPtr<nsIFile> ourselves;
      if (executable &&
          // Despite the name, this really just fetches an nsIFile...
          NS_SUCCEEDED(NS_GetSpecialDirectory(XRE_EXECUTABLE_FILE,
                                              getter_AddRefs(ourselves)))) {
        ourselves = nsMIMEInfoBase::GetCanonicalExecutable(ourselves);
        executable = nsMIMEInfoBase::GetCanonicalExecutable(executable);
        bool isSameApp = false;
        alwaysAsk =
            NS_FAILED(executable->Equals(ourselves, &isSameApp)) || isSameApp;
      }
    }
  }

  // if we were told that we _must_ save to disk without asking, all the stuff
  // before this is irrelevant; override it
  if (mForceSave || mForceSaveInternallyHandled) {
    alwaysAsk = false;
    action = nsIMIMEInfo::saveToDisk;
    shouldAutomaticallyHandleInternally = false;
  }
  // Additionally, if we are asked by the OS to open a local file,
  // automatically downloading it to create a second copy of that file doesn't
  // really make sense. We should ask the user what they want to do.
  if (mSourceUrl->SchemeIs("file") && !alwaysAsk &&
      action == nsIMIMEInfo::saveToDisk) {
    alwaysAsk = true;
  }

  // If adding new checks, make sure this is the last check before telemetry
  // and going ahead with opening the file!
#ifdef XP_WIN
  /* We need to see whether the file we've got here could be
   * executable.  If it could, we had better not try to open it!
   * We can skip this check, though, if we have a setting to open in a
   * helper app.
   */
  if (!alwaysAsk && action != nsIMIMEInfo::saveToDisk &&
      !shouldAutomaticallyHandleInternally) {
    nsCOMPtr<nsIHandlerApp> prefApp;
    mMimeInfo->GetPreferredApplicationHandler(getter_AddRefs(prefApp));
    if (action != nsIMIMEInfo::useHelperApp || !prefApp) {
      nsCOMPtr<nsIFile> fileToTest;
      GetTargetFile(getter_AddRefs(fileToTest));
      if (fileToTest) {
        bool isExecutable;
        rv = fileToTest->IsExecutable(&isExecutable);
        if (NS_FAILED(rv) || mTempFileIsExecutable ||
            isExecutable) {  // checking NS_FAILED, because paranoia is good
          alwaysAsk = true;
        }
      } else {  // Paranoia is good here too, though this really should not
                // happen
        NS_WARNING(
            "GetDownloadInfo returned a null file after the temp file has been "
            "set up! ");
        alwaysAsk = true;
      }
    }
  }
#endif

  if (alwaysAsk) {
    // Display the dialog
    mDialog = do_CreateInstance(NS_HELPERAPPLAUNCHERDLG_CONTRACTID, &rv);
    NS_ENSURE_SUCCESS(rv, rv);

    // this will create a reference cycle (the dialog holds a reference to us as
    // nsIHelperAppLauncher), which will be broken in Cancel or CreateTransfer.
    nsCOMPtr<nsIInterfaceRequestor> dialogParent = GetDialogParent();
    // Don't pop up the downloads panel since we're already going to pop up the
    // UCT dialog for basically the same effect.
    mDialogShowing = true;
    rv = mDialog->Show(this, dialogParent, mReason);

    // what do we do if the dialog failed? I guess we should call Cancel and
    // abort the load....
  } else {
    // We need to do the save/open immediately, then.
    if (action == nsIMIMEInfo::useHelperApp ||
        action == nsIMIMEInfo::useSystemDefault ||
        shouldAutomaticallyHandleInternally) {
      // Check if the file is local, in which case just launch it from where it
      // is. Otherwise, set the file to launch once it's finished downloading.
      rv = mIsFileChannel ? LaunchLocalFile()
                          : SetDownloadToLaunch(
                                shouldAutomaticallyHandleInternally, nullptr);
    } else {
      rv = PromptForSaveDestination();
    }
  }
  return NS_OK;
}

bool nsExternalAppHandler::IsDownloadSpam(nsIChannel* aChannel) {
  nsCOMPtr<nsILoadInfo> loadInfo = aChannel->LoadInfo();
  nsCOMPtr<nsIPermissionManager> permissionManager =
      mozilla::services::GetPermissionManager();
  nsCOMPtr<nsIPrincipal> principal = loadInfo->TriggeringPrincipal();
  bool exactHostMatch = false;
  constexpr auto type = "automatic-download"_ns;
  nsCOMPtr<nsIPermission> permission;

  permissionManager->GetPermissionObject(principal, type, exactHostMatch,
                                         getter_AddRefs(permission));

  if (permission) {
    uint32_t capability;
    permission->GetCapability(&capability);
    if (capability == nsIPermissionManager::DENY_ACTION) {
      mCanceled = true;
      aChannel->Cancel(NS_ERROR_ABORT);
      return true;
    }
    if (capability == nsIPermissionManager::ALLOW_ACTION) {
      return false;
    }
    // If no action is set (i.e: null), we set PROMPT_ACTION by default,
    // which will notify the Downloads UI to open the panel on the next request.
    if (capability == nsIPermissionManager::PROMPT_ACTION) {
      nsCOMPtr<nsIObserverService> observerService =
          mozilla::services::GetObserverService();
      RefPtr<BrowsingContext> browsingContext;
      loadInfo->GetBrowsingContext(getter_AddRefs(browsingContext));

      nsAutoCString cStringURI;
      loadInfo->TriggeringPrincipal()->GetPrePath(cStringURI);
      observerService->NotifyObservers(
          browsingContext, "blocked-automatic-download",
          NS_ConvertASCIItoUTF16(cStringURI.get()).get());
      // FIXME: In order to escape memory leaks, currently we cancel blocked
      // downloads. This is temporary solution, because download data should be
      // kept in order to restart the blocked download.
      mCanceled = true;
      aChannel->Cancel(NS_ERROR_ABORT);
      // End cancel
      return true;
    }
  }
  if (!loadInfo->GetHasValidUserGestureActivation()) {
    permissionManager->AddFromPrincipal(
        principal, type, nsIPermissionManager::PROMPT_ACTION,
        nsIPermissionManager::EXPIRE_NEVER, 0 /* expire time */);
  }

  return false;
}

// Convert error info into proper message text and send OnStatusChange
// notification to the dialog progress listener or nsITransfer implementation.
void nsExternalAppHandler::SendStatusChange(ErrorType type, nsresult rv,
                                            nsIRequest* aRequest,
                                            const nsString& path) {
  const char* msgId = nullptr;
  switch (rv) {
    case NS_ERROR_OUT_OF_MEMORY:
      // No memory
      msgId = "noMemory";
      break;

    case NS_ERROR_FILE_NO_DEVICE_SPACE:
      // Out of space on target volume.
      msgId = "diskFull";
      break;

    case NS_ERROR_FILE_READ_ONLY:
      // Attempt to write to read/only file.
      msgId = "readOnly";
      break;

    case NS_ERROR_FILE_ACCESS_DENIED:
      if (type == kWriteError) {
        // Attempt to write without sufficient permissions.
#if defined(ANDROID)
        // On Android this means the SD card is present but
        // unavailable (read-only).
        msgId = "SDAccessErrorCardReadOnly";
#else
        msgId = "accessError";
#endif
      } else {
        msgId = "launchError";
      }
      break;

    case NS_ERROR_FILE_NOT_FOUND:
    case NS_ERROR_FILE_UNRECOGNIZED_PATH:
      // Helper app not found, let's verify this happened on launch
      if (type == kLaunchError) {
        msgId = "helperAppNotFound";
        break;
      }
#if defined(ANDROID)
      else if (type == kWriteError) {
        // On Android this means the SD card is missing (not in
        // SD slot).
        msgId = "SDAccessErrorCardMissing";
        break;
      }
#endif
      [[fallthrough]];

    default:
      // Generic read/write/launch error message.
      switch (type) {
        case kReadError:
          msgId = "readError";
          break;
        case kWriteError:
          msgId = "writeError";
          break;
        case kLaunchError:
          msgId = "launchError";
          break;
      }
      break;
  }

  MOZ_LOG(
      nsExternalHelperAppService::sLog, LogLevel::Error,
      ("Error: %s, type=%i, listener=0x%p, transfer=0x%p, rv=0x%08" PRIX32 "\n",
       msgId, type, mDialogProgressListener.get(), mTransfer.get(),
       static_cast<uint32_t>(rv)));

  MOZ_LOG(nsExternalHelperAppService::sLog, LogLevel::Error,
          ("       path='%s'\n", NS_ConvertUTF16toUTF8(path).get()));

  // Get properties file bundle and extract status string.
  nsCOMPtr<nsIStringBundleService> stringService =
      mozilla::components::StringBundle::Service();
  if (stringService) {
    nsCOMPtr<nsIStringBundle> bundle;
    if (NS_SUCCEEDED(stringService->CreateBundle(
            "chrome://global/locale/nsWebBrowserPersist.properties",
            getter_AddRefs(bundle)))) {
      nsAutoString msgText;
      AutoTArray<nsString, 1> strings = {path};
      if (NS_SUCCEEDED(bundle->FormatStringFromName(msgId, strings, msgText))) {
        if (mDialogProgressListener) {
          // We have a listener, let it handle the error.
          mDialogProgressListener->OnStatusChange(
              nullptr, (type == kReadError) ? aRequest : nullptr, rv,
              msgText.get());
        } else if (mTransfer) {
          mTransfer->OnStatusChange(nullptr,
                                    (type == kReadError) ? aRequest : nullptr,
                                    rv, msgText.get());
        } else if (XRE_IsParentProcess()) {
          // We don't have a listener.  Simply show the alert ourselves.
          nsCOMPtr<nsIInterfaceRequestor> dialogParent = GetDialogParent();
          nsresult qiRv;
          nsCOMPtr<nsIPrompt> prompter(do_GetInterface(dialogParent, &qiRv));
          nsAutoString title;
          bundle->FormatStringFromName("title", strings, title);

          MOZ_LOG(
              nsExternalHelperAppService::sLog, LogLevel::Debug,
              ("mBrowsingContext=0x%p, prompter=0x%p, qi rv=0x%08" PRIX32
               ", title='%s', msg='%s'",
               mBrowsingContext.get(), prompter.get(),
               static_cast<uint32_t>(qiRv), NS_ConvertUTF16toUTF8(title).get(),
               NS_ConvertUTF16toUTF8(msgText).get()));

          // If we didn't have a prompter we will try and get a window
          // instead, get it's docshell and use it to alert the user.
          if (!prompter) {
            nsCOMPtr<nsPIDOMWindowOuter> window(do_GetInterface(dialogParent));
            if (!window || !window->GetDocShell()) {
              return;
            }

            prompter = do_GetInterface(window->GetDocShell(), &qiRv);

            MOZ_LOG(nsExternalHelperAppService::sLog, LogLevel::Debug,
                    ("No prompter from mBrowsingContext, using DocShell, "
                     "window=0x%p, docShell=0x%p, "
                     "prompter=0x%p, qi rv=0x%08" PRIX32,
                     window.get(), window->GetDocShell(), prompter.get(),
                     static_cast<uint32_t>(qiRv)));

            // If we still don't have a prompter, there's nothing else we
            // can do so just return.
            if (!prompter) {
              MOZ_LOG(nsExternalHelperAppService::sLog, LogLevel::Error,
                      ("No prompter from DocShell, no way to alert user"));
              return;
            }
          }

          // We should always have a prompter at this point.
          prompter->Alert(title.get(), msgText.get());
        }
      }
    }
  }
}

NS_IMETHODIMP
nsExternalAppHandler::OnDataAvailable(nsIRequest* request,
                                      nsIInputStream* inStr,
                                      uint64_t sourceOffset, uint32_t count) {
  nsresult rv = NS_OK;
  // first, check to see if we've been canceled....
  if (mCanceled || !mSaver) {
    // then go cancel our underlying channel too
    return request->Cancel(NS_BINDING_ABORTED);
  }

  // read the data out of the stream and write it to the temp file.
  if (count > 0) {
    mProgress += count;

    nsCOMPtr<nsIStreamListener> saver = do_QueryInterface(mSaver);
    rv = saver->OnDataAvailable(request, inStr, sourceOffset, count);
    if (NS_SUCCEEDED(rv)) {
      // Send progress notification.
      if (mTransfer) {
        mTransfer->OnProgressChange64(nullptr, request, mProgress,
                                      mContentLength, mProgress,
                                      mContentLength);
      }
    } else {
      // An error occurred, notify listener.
      nsAutoString tempFilePath;
      if (mTempFile) {
        mTempFile->GetPath(tempFilePath);
      }
      SendStatusChange(kReadError, rv, request, tempFilePath);

      // Cancel the download.
      Cancel(rv);
    }
  }
  return rv;
}

NS_IMETHODIMP nsExternalAppHandler::OnStopRequest(nsIRequest* request,
                                                  nsresult aStatus) {
  LOG("nsExternalAppHandler::OnStopRequest\n"
      "  mCanceled=%d, mTransfer=0x%p, aStatus=0x%08" PRIX32 "\n",
      mCanceled, mTransfer.get(), static_cast<uint32_t>(aStatus));

  mStopRequestIssued = true;

  // Cancel if the request did not complete successfully.
  if (!mCanceled && NS_FAILED(aStatus)) {
    // Send error notification.
    nsAutoString tempFilePath;
    if (mTempFile) mTempFile->GetPath(tempFilePath);
    SendStatusChange(kReadError, aStatus, request, tempFilePath);

    Cancel(aStatus);
  }

  // first, check to see if we've been canceled....
  if (mCanceled || !mSaver) {
    return NS_OK;
  }

  return mSaver->Finish(NS_OK);
}

NS_IMETHODIMP
nsExternalAppHandler::OnTargetChange(nsIBackgroundFileSaver* aSaver,
                                     nsIFile* aTarget) {
  return NS_OK;
}

NS_IMETHODIMP
nsExternalAppHandler::OnSaveComplete(nsIBackgroundFileSaver* aSaver,
                                     nsresult aStatus) {
  LOG("nsExternalAppHandler::OnSaveComplete\n"
      "  aSaver=0x%p, aStatus=0x%08" PRIX32 ", mCanceled=%d, mTransfer=0x%p\n",
      aSaver, static_cast<uint32_t>(aStatus), mCanceled, mTransfer.get());

  if (!mCanceled) {
    // Save the hash and signature information
    (void)mSaver->GetSha256Hash(mHash);
    (void)mSaver->GetSignatureInfo(mSignatureInfo);

    // Free the reference that the saver keeps on us, even if we couldn't get
    // the hash.
    mSaver = nullptr;

    // Save the redirect information.
    nsCOMPtr<nsIChannel> channel = do_QueryInterface(mRequest);
    if (channel) {
      nsCOMPtr<nsILoadInfo> loadInfo = channel->LoadInfo();
      nsresult rv = NS_OK;
      nsCOMPtr<nsIMutableArray> redirectChain =
          do_CreateInstance(NS_ARRAY_CONTRACTID, &rv);
      NS_ENSURE_SUCCESS(rv, rv);
      LOG("nsExternalAppHandler: Got %zu redirects\n",
          loadInfo->RedirectChain().Length());
      for (nsIRedirectHistoryEntry* entry : loadInfo->RedirectChain()) {
        redirectChain->AppendElement(entry);
      }
      mRedirects = redirectChain;
    }

    if (NS_FAILED(aStatus)) {
      nsAutoString path;
      mTempFile->GetPath(path);

      // It may happen when e10s is enabled that there will be no transfer
      // object available to communicate status as expected by the system.
      // Let's try and create a temporary transfer object to take care of this
      // for us, we'll fall back to using the prompt service if we absolutely
      // have to.
      if (!mTransfer) {
        // We don't care if this fails.
        CreateFailedTransfer();
      }

      SendStatusChange(kWriteError, aStatus, nullptr, path);
      if (!mCanceled) Cancel(aStatus);
      return NS_OK;
    }
  }

  // Notify the transfer object that we are done if the user has chosen an
  // action. If the user hasn't chosen an action, the progress listener
  // (nsITransfer) will be notified in CreateTransfer.
  if (mTransfer) {
    NotifyTransfer(aStatus);
  }

  return NS_OK;
}

void nsExternalAppHandler::NotifyTransfer(nsresult aStatus) {
  MOZ_ASSERT(NS_IsMainThread(), "Must notify on main thread");
  MOZ_ASSERT(mTransfer, "We must have an nsITransfer");

  LOG("Notifying progress listener");

  if (NS_SUCCEEDED(aStatus)) {
    (void)mTransfer->SetSha256Hash(mHash);
    (void)mTransfer->SetSignatureInfo(mSignatureInfo);
    (void)mTransfer->SetRedirects(mRedirects);
    (void)mTransfer->OnProgressChange64(
        nullptr, nullptr, mProgress, mContentLength, mProgress, mContentLength);
  }

  (void)mTransfer->OnStateChange(nullptr, nullptr,
                                 nsIWebProgressListener::STATE_STOP |
                                     nsIWebProgressListener::STATE_IS_REQUEST |
                                     nsIWebProgressListener::STATE_IS_NETWORK,
                                 aStatus);

  // This nsITransfer object holds a reference to us (we are its observer), so
  // we need to release the reference to break a reference cycle (and therefore
  // to prevent leaking).  We do this even if the previous calls failed.
  mTransfer = nullptr;
}

NS_IMETHODIMP nsExternalAppHandler::GetMIMEInfo(nsIMIMEInfo** aMIMEInfo) {
  *aMIMEInfo = mMimeInfo;
  NS_ADDREF(*aMIMEInfo);
  return NS_OK;
}

NS_IMETHODIMP nsExternalAppHandler::GetSource(nsIURI** aSourceURI) {
  NS_ENSURE_ARG(aSourceURI);
  *aSourceURI = mSourceUrl;
  NS_IF_ADDREF(*aSourceURI);
  return NS_OK;
}

NS_IMETHODIMP nsExternalAppHandler::GetSuggestedFileName(
    nsAString& aSuggestedFileName) {
  aSuggestedFileName = mSuggestedFileName;
  return NS_OK;
}

nsresult nsExternalAppHandler::CreateTransfer() {
  LOG("nsExternalAppHandler::CreateTransfer");

  MOZ_ASSERT(NS_IsMainThread(), "Must create transfer on main thread");
  // We are back from the helper app dialog (where the user chooses to save or
  // open), but we aren't done processing the load. in this case, throw up a
  // progress dialog so the user can see what's going on.
  // Also, release our reference to mDialog. We don't need it anymore, and we
  // need to break the reference cycle.
  mDialog = nullptr;
  if (!mDialogProgressListener) {
    NS_WARNING("The dialog should nullify the dialog progress listener");
  }
  // In case of a non acceptable download, we need to cancel the request and
  // pass a FailedTransfer for the Download UI.
  if (mDownloadClassification != nsITransfer::DOWNLOAD_ACCEPTABLE) {
    mCanceled = true;
    mRequest->Cancel(NS_ERROR_ABORT);
    if (mSaver) {
      mSaver->Finish(NS_ERROR_ABORT);
      mSaver = nullptr;
    }
    return CreateFailedTransfer();
  }
  nsresult rv;

  // We must be able to create an nsITransfer object. If not, it doesn't matter
  // much that we can't launch the helper application or save to disk. Work on
  // a local copy rather than mTransfer until we know we succeeded, to make it
  // clearer that this function is re-entrant.
  nsCOMPtr<nsITransfer> transfer =
      do_CreateInstance(NS_TRANSFER_CONTRACTID, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  // Initialize the download
  nsCOMPtr<nsIURI> target;
  rv = NS_NewFileURI(getter_AddRefs(target), mFinalFileDestination);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIChannel> channel = do_QueryInterface(mRequest);
  nsCOMPtr<nsIHttpChannel> httpChannel = do_QueryInterface(mRequest);
  nsCOMPtr<nsIReferrerInfo> referrerInfo = nullptr;
  if (httpChannel) {
    referrerInfo = httpChannel->GetReferrerInfo();
  }

  if (mBrowsingContext) {
    rv = transfer->InitWithBrowsingContext(
        mSourceUrl, target, u""_ns, mMimeInfo, mTimeDownloadStarted, mTempFile,
        this, channel && NS_UsePrivateBrowsing(channel),
        mDownloadClassification, referrerInfo, !mDialogShowing,
        mBrowsingContext, mHandleInternally, nullptr);
  } else {
    rv = transfer->Init(mSourceUrl, nullptr, target, u""_ns, mMimeInfo,
                        mTimeDownloadStarted, mTempFile, this,
                        channel && NS_UsePrivateBrowsing(channel),
                        mDownloadClassification, referrerInfo, !mDialogShowing);
  }
  mDialogShowing = false;

  NS_ENSURE_SUCCESS(rv, rv);

  // If we were cancelled since creating the transfer, just return. It is
  // always ok to return NS_OK if we are cancelled. Callers of this function
  // must call Cancel if CreateTransfer fails, but there's no need to cancel
  // twice.
  if (mCanceled) {
    return NS_OK;
  }
  rv = transfer->OnStateChange(nullptr, mRequest,
                               nsIWebProgressListener::STATE_START |
                                   nsIWebProgressListener::STATE_IS_REQUEST |
                                   nsIWebProgressListener::STATE_IS_NETWORK,
                               NS_OK);
  NS_ENSURE_SUCCESS(rv, rv);

  if (mCanceled) {
    return NS_OK;
  }

  mRequest = nullptr;
  // Finally, save the transfer to mTransfer.
  mTransfer = transfer;
  transfer = nullptr;

  // While we were bringing up the progress dialog, we actually finished
  // processing the url. If that's the case then mStopRequestIssued will be
  // true and OnSaveComplete has been called.
  if (mStopRequestIssued && !mSaver && mTransfer) {
    NotifyTransfer(NS_OK);
  }

  return rv;
}

nsresult nsExternalAppHandler::CreateFailedTransfer() {
  nsresult rv;
  nsCOMPtr<nsITransfer> transfer =
      do_CreateInstance(NS_TRANSFER_CONTRACTID, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  // We won't pass the temp file to the transfer, so if we have one it needs to
  // get deleted now.
  if (mTempFile) {
    if (mSaver) {
      mSaver->Finish(NS_BINDING_ABORTED);
      mSaver = nullptr;
    }
    mTempFile->Remove(false);
  }

  nsCOMPtr<nsIURI> pseudoTarget;
  if (!mFinalFileDestination) {
    // If we don't have a download directory we're kinda screwed but it's OK
    // we'll still report the error via the prompter.
    auto res = GetInitialDownloadDirectory(true);
    if (res.isErr()) return res.unwrapErr();
    nsCOMPtr<nsIFile> pseudoFile = res.unwrap();

    // Append the default suggested filename. If the user restarts the transfer
    // we will re-trigger a filename check anyway to ensure that it is unique.
    rv = pseudoFile->Append(mSuggestedFileName);
    NS_ENSURE_SUCCESS(rv, rv);

    rv = NS_NewFileURI(getter_AddRefs(pseudoTarget), pseudoFile);
    NS_ENSURE_SUCCESS(rv, rv);
  } else {
    // Initialize the target, if present
    rv = NS_NewFileURI(getter_AddRefs(pseudoTarget), mFinalFileDestination);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  nsCOMPtr<nsIChannel> channel = do_QueryInterface(mRequest);
  nsCOMPtr<nsIHttpChannel> httpChannel = do_QueryInterface(mRequest);
  nsCOMPtr<nsIReferrerInfo> referrerInfo = nullptr;
  if (httpChannel) {
    referrerInfo = httpChannel->GetReferrerInfo();
  }

  if (mBrowsingContext) {
    rv = transfer->InitWithBrowsingContext(
        mSourceUrl, pseudoTarget, u""_ns, mMimeInfo, mTimeDownloadStarted,
        mTempFile, this, channel && NS_UsePrivateBrowsing(channel),
        mDownloadClassification, referrerInfo, true, mBrowsingContext,
        mHandleInternally, httpChannel);
  } else {
    rv = transfer->Init(mSourceUrl, nullptr, pseudoTarget, u""_ns, mMimeInfo,
                        mTimeDownloadStarted, mTempFile, this,
                        channel && NS_UsePrivateBrowsing(channel),
                        mDownloadClassification, referrerInfo, true);
  }
  NS_ENSURE_SUCCESS(rv, rv);

  // Our failed transfer is ready.
  mTransfer = std::move(transfer);

  return NS_OK;
}

nsresult nsExternalAppHandler::SaveDestinationAvailable(nsIFile* aFile,
                                                        bool aDialogWasShown) {
  if (aFile) {
    if (aDialogWasShown) {
      mDialogShowing = true;
    }
    ContinueSave(aFile);
  } else {
    Cancel(NS_BINDING_ABORTED);
  }

  return NS_OK;
}

void nsExternalAppHandler::RequestSaveDestination(
    const nsString& aDefaultFile, const nsString& aFileExtension) {
  // Display the dialog
  // XXX Convert to use file picker? No, then embeddors could not do any sort of
  // "AutoDownload" w/o showing a prompt
  nsresult rv = NS_OK;
  if (!mDialog) {
    // Get helper app launcher dialog.
    mDialog = do_CreateInstance(NS_HELPERAPPLAUNCHERDLG_CONTRACTID, &rv);
    if (rv != NS_OK) {
      Cancel(NS_BINDING_ABORTED);
      return;
    }
  }

  // we want to explicitly unescape aDefaultFile b4 passing into the dialog. we
  // can't unescape it because the dialog is implemented by a JS component which
  // doesn't have a window so no unescape routine is defined...

  // Now, be sure to keep |this| alive, and the dialog
  // If we don't do this, users that close the helper app dialog while the file
  // picker is up would cause Cancel() to be called, and the dialog would be
  // released, which would release this object too, which would crash.
  // See Bug 249143
  RefPtr<nsExternalAppHandler> kungFuDeathGrip(this);
  nsCOMPtr<nsIHelperAppLauncherDialog> dlg(mDialog);
  nsCOMPtr<nsIInterfaceRequestor> dialogParent = GetDialogParent();

  rv = dlg->PromptForSaveToFileAsync(this, dialogParent, aDefaultFile.get(),
                                     aFileExtension.get(), mForceSave);
  if (NS_FAILED(rv)) {
    Cancel(NS_BINDING_ABORTED);
  }
}

// PromptForSaveDestination should only be called by the helper app dialog which
// allows the user to say launch with application or save to disk.
NS_IMETHODIMP nsExternalAppHandler::PromptForSaveDestination() {
  if (mCanceled) return NS_OK;

  if (mForceSave || mForceSaveInternallyHandled) {
    mMimeInfo->SetPreferredAction(nsIMIMEInfo::saveToDisk);
  }

  if (mSuggestedFileName.IsEmpty()) {
    RequestSaveDestination(mTempLeafName, mFileExtension);
  } else {
    nsAutoString fileExt;
    int32_t pos = mSuggestedFileName.RFindChar('.');
    if (pos >= 0) {
      mSuggestedFileName.Right(fileExt, mSuggestedFileName.Length() - pos);
    }
    if (fileExt.IsEmpty()) {
      fileExt = mFileExtension;
    }

    RequestSaveDestination(mSuggestedFileName, fileExt);
  }

  return NS_OK;
}
nsresult nsExternalAppHandler::ContinueSave(nsIFile* aNewFileLocation) {
  if (mCanceled) return NS_OK;

  MOZ_ASSERT(aNewFileLocation, "Must be called with a non-null file");

  int32_t action = nsIMIMEInfo::saveToDisk;
  mMimeInfo->GetPreferredAction(&action);
  mHandleInternally = action == nsIMIMEInfo::handleInternally;

  nsresult rv = NS_OK;
  nsCOMPtr<nsIFile> fileToUse = aNewFileLocation;
  mFinalFileDestination = fileToUse;

  // Move what we have in the final directory, but append .part
  // to it, to indicate that it's unfinished.  Do not call SetTarget on the
  // saver if we are done (Finish has been called) but OnSaverComplete has
  // not been called.
  if (mFinalFileDestination && mSaver && !mStopRequestIssued) {
    nsCOMPtr<nsIFile> movedFile;
    mFinalFileDestination->Clone(getter_AddRefs(movedFile));
    if (movedFile) {
      nsAutoCString randomChars;
      rv = GenerateRandomName(randomChars);
      if (NS_SUCCEEDED(rv)) {
        // Get the leaf name, strip any extensions, then
        // add random bytes, followed by the extensions and '.part'.
        nsAutoString leafName;
        mFinalFileDestination->GetLeafName(leafName);
        auto nameWithoutExtensionLength = leafName.FindChar('.');
        nsAutoString extensions(u"");
        if (nameWithoutExtensionLength == kNotFound) {
          nameWithoutExtensionLength = leafName.Length();
        } else {
          extensions = Substring(leafName, nameWithoutExtensionLength);
        }
        leafName.Truncate(nameWithoutExtensionLength);

        nsAutoString suffix = u"."_ns + NS_ConvertASCIItoUTF16(randomChars) +
                              extensions + u".part"_ns;
#ifdef XP_WIN
        // Deal with MAX_PATH on Windows. Worth noting that the original
        // path for mFinalFileDestination must be valid for us to get
        // here: either SetDownloadToLaunch or the caller of
        // SaveDestinationAvailable has called CreateUnique or similar
        // to ensure both a unique name and one that isn't too long.
        // The only issue is we're making it longer to get the part
        // file path...
        nsAutoString path;
        mFinalFileDestination->GetPath(path);
        CheckedInt<uint16_t> fullPathLength =
            CheckedInt<uint16_t>(path.Length()) + 1 + randomChars.Length() +
            std::size(".part");
        if (!fullPathLength.isValid()) {
          leafName.Truncate();
        } else if (fullPathLength.value() > MAX_PATH) {
          int32_t leafNameRemaining =
              (int32_t)leafName.Length() - (fullPathLength.value() - MAX_PATH);
          leafName.Truncate(std::max(leafNameRemaining, 0));
        }
#endif
        leafName.Append(suffix);
        movedFile->SetLeafName(leafName);

        rv = mSaver->SetTarget(movedFile, true);
        if (NS_FAILED(rv)) {
          nsAutoString path;
          mTempFile->GetPath(path);
          SendStatusChange(kWriteError, rv, nullptr, path);
          Cancel(rv);
          return NS_OK;
        }

        mTempFile = movedFile;
      }
    }
  }

  // The helper app dialog has told us what to do and we have a final file
  // destination.
  rv = CreateTransfer();
  // If we fail to create the transfer, Cancel.
  if (NS_FAILED(rv)) {
    Cancel(rv);
    return rv;
  }

  return NS_OK;
}

// SetDownloadToLaunch should only be called by the helper app dialog which
// allows the user to say launch with application or save to disk.
NS_IMETHODIMP nsExternalAppHandler::SetDownloadToLaunch(
    bool aHandleInternally, nsIFile* aNewFileLocation) {
  if (mCanceled) return NS_OK;

  mHandleInternally = aHandleInternally;

  // Now that the user has elected to launch the downloaded file with a helper
  // app, we're justified in removing the 'salted' name.  We'll rename to what
  // was specified in mSuggestedFileName after the download is done prior to
  // launching the helper app.  So that any existing file of that name won't be
  // overwritten we call CreateUnique().  Also note that we use the same
  // directory as originally downloaded so the download can be renamed in place
  // later.
  nsCOMPtr<nsIFile> fileToUse;
  if (aNewFileLocation) {
    fileToUse = aNewFileLocation;
  } else {
    auto res = GetInitialDownloadDirectory();
    if (res.isErr()) return res.unwrapErr();
    fileToUse = res.unwrap();

    if (mSuggestedFileName.IsEmpty()) {
      // Keep using the leafname of the temp file, since we're just starting a
      // helper
      mSuggestedFileName = mTempLeafName;
    }

#ifdef XP_WIN
    // Ensure we don't double-append the file extension if it matches:
    if (StringEndsWith(mSuggestedFileName, mFileExtension,
                       nsCaseInsensitiveStringComparator)) {
      fileToUse->Append(mSuggestedFileName);
    } else {
      fileToUse->Append(mSuggestedFileName + mFileExtension);
    }
#else
    fileToUse->Append(mSuggestedFileName);
#endif
  }

  nsresult rv = fileToUse->CreateUnique(nsIFile::NORMAL_FILE_TYPE, 0600);
  if (NS_SUCCEEDED(rv)) {
    mFinalFileDestination = fileToUse;
    // launch the progress window now that the user has picked the desired
    // action.
    rv = CreateTransfer();
    if (NS_FAILED(rv)) {
      Cancel(rv);
    }
  } else {
    // Cancel the download and report an error.  We do not want to end up in
    // a state where it appears that we have a normal download that is
    // pointing to a file that we did not actually create.
    nsAutoString path;
    mTempFile->GetPath(path);
    SendStatusChange(kWriteError, rv, nullptr, path);
    Cancel(rv);
  }
  return rv;
}

nsresult nsExternalAppHandler::LaunchLocalFile() {
  nsCOMPtr<nsIFileURL> fileUrl(do_QueryInterface(mSourceUrl));
  if (!fileUrl) {
    return NS_OK;
  }
  Cancel(NS_BINDING_ABORTED);
  nsCOMPtr<nsIFile> file;
  nsresult rv = fileUrl->GetFile(getter_AddRefs(file));

  if (NS_SUCCEEDED(rv)) {
    rv = mMimeInfo->LaunchWithFile(file);
    if (NS_SUCCEEDED(rv)) return NS_OK;
  }
  nsAutoString path;
  if (file) file->GetPath(path);
  // If we get here, an error happened
  SendStatusChange(kLaunchError, rv, nullptr, path);
  return rv;
}

NS_IMETHODIMP nsExternalAppHandler::Cancel(nsresult aReason) {
  NS_ENSURE_ARG(NS_FAILED(aReason));

  if (mCanceled) {
    return NS_OK;
  }
  mCanceled = true;

  if (mSaver) {
    // We are still writing to the target file.  Give the saver a chance to
    // close the target file, then notify the transfer object if necessary in
    // the OnSaveComplete callback.
    mSaver->Finish(aReason);
    mSaver = nullptr;
  } else {
    if (mStopRequestIssued && mTempFile) {
      // This branch can only happen when the user cancels the helper app dialog
      // when the request has completed. The temp file has to be removed here,
      // because mSaver has been released at that time with the temp file left.
      (void)mTempFile->Remove(false);
    }

    // Notify the transfer object that the download has been canceled, if the
    // user has already chosen an action and we didn't notify already.
    if (mTransfer) {
      NotifyTransfer(aReason);
    }
  }

  // Break our reference cycle with the helper app dialog (set up in
  // OnStartRequest)
  mDialog = nullptr;
  mDialogShowing = false;

  mRequest = nullptr;

  // Release the listener, to break the reference cycle with it (we are the
  // observer of the listener).
  mDialogProgressListener = nullptr;

  return NS_OK;
}

bool nsExternalAppHandler::GetNeverAskFlagFromPref(const char* prefName,
                                                   const char* aContentType) {
  // Search the obsolete pref strings.
  nsAutoCString prefCString;
  Preferences::GetCString(prefName, prefCString);
  if (prefCString.IsEmpty()) {
    // Default is true, if not found in the pref string.
    return true;
  }

  NS_UnescapeURL(prefCString);
  nsACString::const_iterator start, end;
  prefCString.BeginReading(start);
  prefCString.EndReading(end);
  return !CaseInsensitiveFindInReadable(nsDependentCString(aContentType), start,
                                        end);
}

NS_IMETHODIMP
nsExternalAppHandler::GetName(nsACString& aName) {
  aName.AssignLiteral("nsExternalAppHandler");
  return NS_OK;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////
// The following section contains our nsIMIMEService implementation and related
// methods.
//
//////////////////////////////////////////////////////////////////////////////////////////////////////////////

// nsIMIMEService methods
NS_IMETHODIMP nsExternalHelperAppService::GetFromTypeAndExtension(
    const nsACString& aMIMEType, const nsACString& aFileExt,
    nsIMIMEInfo** _retval) {
  MOZ_ASSERT(!aMIMEType.IsEmpty() || !aFileExt.IsEmpty(),
             "Give me something to work with");
  MOZ_DIAGNOSTIC_ASSERT(aFileExt.FindChar('\0') == kNotFound,
                        "The extension should never contain null characters");
  LOG("Getting mimeinfo from type '%s' ext '%s'\n",
      PromiseFlatCString(aMIMEType).get(), PromiseFlatCString(aFileExt).get());

  *_retval = nullptr;

  // OK... we need a type. Get one.
  nsAutoCString typeToUse(aMIMEType);
  if (typeToUse.IsEmpty()) {
    nsresult rv = GetTypeFromExtension(aFileExt, typeToUse);
    if (NS_FAILED(rv)) return NS_ERROR_NOT_AVAILABLE;
  }

  // We promise to only send lower case mime types to the OS
  ToLowerCase(typeToUse);

  // First, ask the OS for a mime info
  bool found;
  nsresult rv = GetMIMEInfoFromOS(typeToUse, aFileExt, &found, _retval);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  LOG("OS gave back 0x%p - found: %i\n", *_retval, found);
  // If we got no mimeinfo, something went wrong. Probably lack of memory.
  if (!*_retval) return NS_ERROR_OUT_OF_MEMORY;

  // The handler service can make up for bad mime types by checking the file
  // extension. If the mime type is known (in extras or in the handler
  // service), we stop it doing so by flipping this bool to true.
  bool trustMIMEType = false;

  // Check extras - not everything we support will be known by the OS store,
  // unfortunately, and it may even miss some extensions that we know should
  // be accepted. We only do this for non-octet-stream mimetypes, because
  // our information for octet-stream would lead to us trying to open all such
  // files as Binary file with exe, com or bin extension regardless of the
  // real extension.
  if (!typeToUse.Equals(APPLICATION_OCTET_STREAM,
                        nsCaseInsensitiveCStringComparator)) {
    rv = FillMIMEInfoForMimeTypeFromExtras(typeToUse, !found, *_retval);
    LOG("Searched extras (by type), rv 0x%08" PRIX32 "\n",
        static_cast<uint32_t>(rv));
    trustMIMEType = NS_SUCCEEDED(rv);
    found = found || NS_SUCCEEDED(rv);
  }

  // Now, let's see if we can find something in our datastore.
  // This will not overwrite the OS information that interests us
  // (i.e. default application, default app. description)
  nsCOMPtr<nsIHandlerService> handlerSvc =
      do_GetService(NS_HANDLERSERVICE_CONTRACTID);
  if (handlerSvc) {
    bool hasHandler = false;
    (void)handlerSvc->Exists(*_retval, &hasHandler);
    if (hasHandler) {
      rv = handlerSvc->FillHandlerInfo(*_retval, ""_ns);
      LOG("Data source: Via type: retval 0x%08" PRIx32 "\n",
          static_cast<uint32_t>(rv));
      trustMIMEType = trustMIMEType || NS_SUCCEEDED(rv);
    } else {
      rv = NS_ERROR_NOT_AVAILABLE;
    }

    found = found || NS_SUCCEEDED(rv);
  }

  // If we still haven't found anything, try finding a match for
  // an extension in extras first:
  if (!found && !aFileExt.IsEmpty()) {
    rv = FillMIMEInfoForExtensionFromExtras(aFileExt, *_retval);
    LOG("Searched extras (by ext), rv 0x%08" PRIX32 "\n",
        static_cast<uint32_t>(rv));
  }

  // Then check the handler service - but only do so if we really do not know
  // the mimetype. This avoids overwriting good mimetype info with bad file
  // extension info.
  if ((!found || !trustMIMEType) && handlerSvc && !aFileExt.IsEmpty()) {
    nsAutoCString overrideType;
    rv = handlerSvc->GetTypeFromExtension(aFileExt, overrideType);
    if (NS_SUCCEEDED(rv) && !overrideType.IsEmpty()) {
      // We can't check handlerSvc->Exists() here, because we have a
      // overideType. That's ok, it just results in some console noise.
      // (If there's no handler for the override type, it throws)
      rv = handlerSvc->FillHandlerInfo(*_retval, overrideType);
      LOG("Data source: Via ext: retval 0x%08" PRIx32 "\n",
          static_cast<uint32_t>(rv));
      found = found || NS_SUCCEEDED(rv);
    }
  }

  // If we still don't have a match, at least set the file description
  // to `${aFileExt} File` if it's empty:
  if (!found && !aFileExt.IsEmpty()) {
    // XXXzpao This should probably be localized
    nsAutoCString desc(aFileExt);
    desc.AppendLiteral(" File");
    (*_retval)->SetDescription(NS_ConvertUTF8toUTF16(desc));
    LOG("Falling back to 'File' file description\n");
  }

  // Sometimes, OSes give us bad data. We have a set of forbidden extensions
  // for some MIME types. If the primary extension is forbidden,
  // overwrite it with a known-good one. See bug 1571247 for context.
  nsAutoCString primaryExtension;
  (*_retval)->GetPrimaryExtension(primaryExtension);
  if (!primaryExtension.EqualsIgnoreCase(PromiseFlatCString(aFileExt).get())) {
    if (MaybeReplacePrimaryExtension(primaryExtension, *_retval)) {
      (*_retval)->GetPrimaryExtension(primaryExtension);
    }
  }

  // Finally, check if we got a file extension and if yes, if it is an
  // extension on the mimeinfo, in which case we want it to be the primary one
  if (!aFileExt.IsEmpty()) {
    bool matches = false;
    (*_retval)->ExtensionExists(aFileExt, &matches);
    LOG("Extension '%s' matches mime info: %i\n",
        PromiseFlatCString(aFileExt).get(), matches);
    if (matches) {
      nsAutoCString fileExt;
      ToLowerCase(aFileExt, fileExt);
      (*_retval)->SetPrimaryExtension(fileExt);
      primaryExtension = fileExt;
    }
  }

  // Overwrite with a generic description if the primary extension for the
  // type is in our list; these are file formats supported by Firefox and
  // we don't want other brands positioning themselves as the sole viewer
  // for a system.
  if (!primaryExtension.IsEmpty()) {
    for (const char* ext : descriptionOverwriteExtensions) {
      if (primaryExtension.Equals(ext)) {
        nsCOMPtr<nsIStringBundleService> bundleService =
            do_GetService(NS_STRINGBUNDLE_CONTRACTID, &rv);
        NS_ENSURE_SUCCESS(rv, rv);
        nsCOMPtr<nsIStringBundle> unknownContentTypeBundle;
        rv = bundleService->CreateBundle(
            "chrome://mozapps/locale/downloads/unknownContentType.properties",
            getter_AddRefs(unknownContentTypeBundle));
        if (NS_SUCCEEDED(rv)) {
          nsAutoCString stringName(ext);
          stringName.AppendLiteral("ExtHandlerDescription");
          nsAutoString handlerDescription;
          rv = unknownContentTypeBundle->GetStringFromName(stringName.get(),
                                                           handlerDescription);
          if (NS_SUCCEEDED(rv)) {
            (*_retval)->SetDescription(handlerDescription);
          }
        }
        break;
      }
    }
  }

  if (LOG_ENABLED()) {
    nsAutoCString type;
    (*_retval)->GetMIMEType(type);

    LOG("MIME Info Summary: Type '%s', Primary Ext '%s'\n", type.get(),
        primaryExtension.get());
  }

  return NS_OK;
}

bool nsExternalHelperAppService::GetMIMETypeFromDefaultForExtension(
    const nsACString& aExtension, nsACString& aMIMEType) {
  // First of all, check our default entries
  for (auto& entry : defaultMimeEntries) {
    if (aExtension.LowerCaseEqualsASCII(entry.mFileExtension)) {
      aMIMEType = entry.mMimeType;
      return true;
    }
  }
  return false;
}

NS_IMETHODIMP
nsExternalHelperAppService::GetTypeFromExtension(const nsACString& aFileExt,
                                                 nsACString& aContentType) {
  // OK. We want to try the following sources of mimetype information, in this
  // order:
  // 1. defaultMimeEntries array
  // 2. OS-provided information
  // 3. our "extras" array
  // 4. Information from plugins
  // 5. The "ext-to-type-mapping" category
  // Note that, we are intentionally not looking at the handler service, because
  // that can be affected by websites, which leads to undesired behavior.

  // Early return if called with an empty extension parameter
  if (aFileExt.IsEmpty()) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  // First of all, check our default entries
  if (GetMIMETypeFromDefaultForExtension(aFileExt, aContentType)) {
    return NS_OK;
  }

  // Ask OS.
  if (GetMIMETypeFromOSForExtension(aFileExt, aContentType)) {
    return NS_OK;
  }

  // Check extras array.
  bool found = GetTypeFromExtras(aFileExt, aContentType);
  if (found) {
    return NS_OK;
  }

  // Let's see if an extension added something
  nsCOMPtr<nsICategoryManager> catMan(
      do_GetService("@mozilla.org/categorymanager;1"));
  if (catMan) {
    // The extension in the category entry is always stored as lowercase
    nsAutoCString lowercaseFileExt(aFileExt);
    ToLowerCase(lowercaseFileExt);
    // Read the MIME type from the category entry, if available
    nsCString type;
    nsresult rv =
        catMan->GetCategoryEntry("ext-to-type-mapping", lowercaseFileExt, type);
    if (NS_SUCCEEDED(rv)) {
      aContentType = type;
      return NS_OK;
    }
  }

  return NS_ERROR_NOT_AVAILABLE;
}

NS_IMETHODIMP nsExternalHelperAppService::GetPrimaryExtension(
    const nsACString& aMIMEType, const nsACString& aFileExt,
    nsACString& _retval) {
  NS_ENSURE_ARG(!aMIMEType.IsEmpty());

  nsCOMPtr<nsIMIMEInfo> mi;
  nsresult rv =
      GetFromTypeAndExtension(aMIMEType, aFileExt, getter_AddRefs(mi));
  if (NS_FAILED(rv)) return rv;

  return mi->GetPrimaryExtension(_retval);
}

NS_IMETHODIMP nsExternalHelperAppService::GetDefaultTypeFromURI(
    nsIURI* aURI, nsACString& aContentType) {
  NS_ENSURE_ARG_POINTER(aURI);
  nsresult rv = NS_ERROR_NOT_AVAILABLE;
  aContentType.Truncate();

  // Now try to get an nsIURL so we don't have to do our own parsing
  nsCOMPtr<nsIURL> url = do_QueryInterface(aURI);
  if (!url) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  nsAutoCString ext;
  rv = url->GetFileExtension(ext);
  if (NS_FAILED(rv)) {
    return rv;
  }

  if (!ext.IsEmpty()) {
    UnescapeFragment(ext, url, ext);

    if (GetMIMETypeFromDefaultForExtension(ext, aContentType)) {
      return NS_OK;
    }
  }

  return NS_ERROR_NOT_AVAILABLE;
};

NS_IMETHODIMP nsExternalHelperAppService::GetTypeFromURI(
    nsIURI* aURI, nsACString& aContentType) {
  NS_ENSURE_ARG_POINTER(aURI);
  nsresult rv = NS_ERROR_NOT_AVAILABLE;
  aContentType.Truncate();

  // First look for a file to use.  If we have one, we just use that.
  nsCOMPtr<nsIFileURL> fileUrl = do_QueryInterface(aURI);
  if (fileUrl) {
    nsCOMPtr<nsIFile> file;
    rv = fileUrl->GetFile(getter_AddRefs(file));
    if (NS_SUCCEEDED(rv)) {
      rv = GetTypeFromFile(file, aContentType);
      if (NS_SUCCEEDED(rv)) {
        // we got something!
        return rv;
      }
    }
  }

  // Now try to get an nsIURL so we don't have to do our own parsing
  nsCOMPtr<nsIURL> url = do_QueryInterface(aURI);
  if (url) {
    nsAutoCString ext;
    rv = url->GetFileExtension(ext);
    if (NS_FAILED(rv)) return rv;
    if (ext.IsEmpty()) return NS_ERROR_NOT_AVAILABLE;

    UnescapeFragment(ext, url, ext);

    return GetTypeFromExtension(ext, aContentType);
  }

  // no url, let's give the raw spec a shot
  nsAutoCString specStr;
  rv = aURI->GetSpec(specStr);
  if (NS_FAILED(rv)) return rv;
  UnescapeFragment(specStr, aURI, specStr);

  // find the file extension (if any)
  int32_t extLoc = specStr.RFindChar('.');
  int32_t specLength = specStr.Length();
  if (-1 != extLoc && extLoc != specLength - 1 &&
      // nothing over 20 chars long can be sanely considered an
      // extension.... Dat dere would be just data.
      specLength - extLoc < 20) {
    return GetTypeFromExtension(Substring(specStr, extLoc + 1), aContentType);
  }

  // We found no information; say so.
  return NS_ERROR_NOT_AVAILABLE;
}

NS_IMETHODIMP nsExternalHelperAppService::GetTypeFromFile(
    nsIFile* aFile, nsACString& aContentType) {
  NS_ENSURE_ARG_POINTER(aFile);
  nsresult rv;

  // Get the Extension
  nsAutoString fileName;
  rv = aFile->GetLeafName(fileName);
  if (NS_FAILED(rv)) return rv;

  nsAutoCString fileExt;
  if (!fileName.IsEmpty()) {
    int32_t len = fileName.Length();
    for (int32_t i = len; i >= 0; i--) {
      if (fileName[i] == char16_t('.')) {
        CopyUTF16toUTF8(Substring(fileName, i + 1), fileExt);
        break;
      }
    }
  }

  if (fileExt.IsEmpty()) return NS_ERROR_FAILURE;

  return GetTypeFromExtension(fileExt, aContentType);
}

nsresult nsExternalHelperAppService::FillMIMEInfoForMimeTypeFromExtras(
    const nsACString& aContentType, bool aOverwriteDescription,
    nsIMIMEInfo* aMIMEInfo) {
  NS_ENSURE_ARG(aMIMEInfo);

  NS_ENSURE_ARG(!aContentType.IsEmpty());

  // Look for default entry with matching mime type.
  nsAutoCString MIMEType(aContentType);
  ToLowerCase(MIMEType);
  for (auto entry : extraMimeEntries) {
    if (MIMEType.Equals(entry.mMimeType)) {
      // This is the one. Set attributes appropriately.
      nsDependentCString extensions(entry.mFileExtensions);
      nsACString::const_iterator start, end;
      extensions.BeginReading(start);
      extensions.EndReading(end);
      while (start != end) {
        nsACString::const_iterator cursor = start;
        mozilla::Unused << FindCharInReadable(',', cursor, end);
        aMIMEInfo->AppendExtension(Substring(start, cursor));
        // If a comma was found, skip it for the next search.
        start = cursor != end ? ++cursor : cursor;
      }

      nsAutoString desc;
      aMIMEInfo->GetDescription(desc);
      if (aOverwriteDescription || desc.IsEmpty()) {
        aMIMEInfo->SetDescription(NS_ConvertASCIItoUTF16(entry.mDescription));
      }
      return NS_OK;
    }
  }

  return NS_ERROR_NOT_AVAILABLE;
}

nsresult nsExternalHelperAppService::FillMIMEInfoForExtensionFromExtras(
    const nsACString& aExtension, nsIMIMEInfo* aMIMEInfo) {
  nsAutoCString type;
  bool found = GetTypeFromExtras(aExtension, type);
  if (!found) return NS_ERROR_NOT_AVAILABLE;
  return FillMIMEInfoForMimeTypeFromExtras(type, true, aMIMEInfo);
}

bool nsExternalHelperAppService::MaybeReplacePrimaryExtension(
    const nsACString& aPrimaryExtension, nsIMIMEInfo* aMIMEInfo) {
  for (const auto& entry : sForbiddenPrimaryExtensions) {
    if (aPrimaryExtension.LowerCaseEqualsASCII(entry.mFileExtension)) {
      nsDependentCString mime(entry.mMimeType);
      for (const auto& extraEntry : extraMimeEntries) {
        if (mime.LowerCaseEqualsASCII(extraEntry.mMimeType)) {
          nsDependentCString goodExts(extraEntry.mFileExtensions);
          int32_t commaPos = goodExts.FindChar(',');
          commaPos = commaPos == kNotFound ? goodExts.Length() : commaPos;
          auto goodExt = Substring(goodExts, 0, commaPos);
          aMIMEInfo->SetPrimaryExtension(goodExt);
          return true;
        }
      }
    }
  }
  return false;
}

bool nsExternalHelperAppService::GetTypeFromExtras(const nsACString& aExtension,
                                                   nsACString& aMIMEType) {
  NS_ASSERTION(!aExtension.IsEmpty(), "Empty aExtension parameter!");

  // Look for default entry with matching extension.
  nsDependentCString::const_iterator start, end, iter;
  int32_t numEntries = std::size(extraMimeEntries);
  for (int32_t index = 0; index < numEntries; index++) {
    nsDependentCString extList(extraMimeEntries[index].mFileExtensions);
    extList.BeginReading(start);
    extList.EndReading(end);
    iter = start;
    while (start != end) {
      FindCharInReadable(',', iter, end);
      if (Substring(start, iter)
              .Equals(aExtension, nsCaseInsensitiveCStringComparator)) {
        aMIMEType = extraMimeEntries[index].mMimeType;
        return true;
      }
      if (iter != end) {
        ++iter;
      }
      start = iter;
    }
  }

  return false;
}

bool nsExternalHelperAppService::GetMIMETypeFromOSForExtension(
    const nsACString& aExtension, nsACString& aMIMEType) {
  bool found = false;
  nsCOMPtr<nsIMIMEInfo> mimeInfo;
  nsresult rv =
      GetMIMEInfoFromOS(""_ns, aExtension, &found, getter_AddRefs(mimeInfo));
  return NS_SUCCEEDED(rv) && found && mimeInfo &&
         NS_SUCCEEDED(mimeInfo->GetMIMEType(aMIMEType));
}

nsresult nsExternalHelperAppService::GetMIMEInfoFromOS(
    const nsACString& aMIMEType, const nsACString& aFileExt, bool* aFound,
    nsIMIMEInfo** aMIMEInfo) {
  *aMIMEInfo = nullptr;
  *aFound = false;
  return NS_ERROR_NOT_IMPLEMENTED;
}

nsresult nsExternalHelperAppService::UpdateDefaultAppInfo(
    nsIMIMEInfo* aMIMEInfo) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

bool nsExternalHelperAppService::GetFileNameFromChannel(nsIChannel* aChannel,
                                                        nsAString& aFileName,
                                                        nsIURI** aURI) {
  if (!aChannel) {
    return false;
  }

  aChannel->GetURI(aURI);
  nsCOMPtr<nsIURL> url = do_QueryInterface(*aURI);

  // Check if we have a POST request, in which case we don't want to use
  // the url's extension
  bool allowURLExt = !net::ChannelIsPost(aChannel);

  // Check if we had a query string - we don't want to check the URL
  // extension if a query is present in the URI
  // If we already know we don't want to check the URL extension, don't
  // bother checking the query
  if (url && allowURLExt) {
    nsAutoCString query;

    // We only care about the query for HTTP and HTTPS URLs
    if (net::SchemeIsHttpOrHttps(url)) {
      url->GetQuery(query);
    }

    // Only get the extension if the query is empty; if it isn't, then the
    // extension likely belongs to a cgi script and isn't helpful
    allowURLExt = query.IsEmpty();
  }

  aChannel->GetContentDispositionFilename(aFileName);

  return allowURLExt;
}

NS_IMETHODIMP
nsExternalHelperAppService::GetValidFileName(nsIChannel* aChannel,
                                             const nsACString& aType,
                                             nsIURI* aOriginalURI,
                                             uint32_t aFlags,
                                             nsAString& aOutFileName) {
  nsCOMPtr<nsIURI> uri;
  bool allowURLExtension =
      GetFileNameFromChannel(aChannel, aOutFileName, getter_AddRefs(uri));

  nsCOMPtr<nsIMIMEInfo> mimeInfo = ValidateFileNameForSaving(
      aOutFileName, aType, uri, aOriginalURI, aFlags, allowURLExtension);
  return NS_OK;
}

NS_IMETHODIMP
nsExternalHelperAppService::ValidateFileNameForSaving(
    const nsAString& aFileName, const nsACString& aType, uint32_t aFlags,
    nsAString& aOutFileName) {
  nsAutoString fileName(aFileName);

  // Just sanitize the filename only.
  if (aFlags & VALIDATE_SANITIZE_ONLY) {
    SanitizeFileName(fileName, aFlags);
  } else {
    nsCOMPtr<nsIMIMEInfo> mimeInfo = ValidateFileNameForSaving(
        fileName, aType, nullptr, nullptr, aFlags, true);
  }

  aOutFileName = fileName;
  return NS_OK;
}

already_AddRefed<nsIMIMEInfo>
nsExternalHelperAppService::ValidateFileNameForSaving(
    nsAString& aFileName, const nsACString& aMimeType, nsIURI* aURI,
    nsIURI* aOriginalURI, uint32_t aFlags, bool aAllowURLExtension) {
  nsAutoString fileName(aFileName);
  nsAutoCString extension;
  nsCOMPtr<nsIMIMEInfo> mimeInfo;

  bool isBinaryType = aMimeType.EqualsLiteral(APPLICATION_OCTET_STREAM) ||
                      aMimeType.EqualsLiteral(BINARY_OCTET_STREAM) ||
                      aMimeType.EqualsLiteral("application/x-msdownload");

  // We don't want to save hidden files starting with a dot, so remove any
  // leading periods. This is done first, so that the remainder will be
  // treated as the filename, and not an extension.
  // Also, Windows ignores terminating dots. So we have to as well, so
  // that our security checks do "the right thing"
  fileName.Trim(".");

  bool urlIsFile = !!aURI && aURI->SchemeIs("file");

  // We get the mime service here even though we're the default implementation
  // of it, so it's possible to override only the mime service and not need to
  // reimplement the whole external helper app service itself.
  nsCOMPtr<nsIMIMEService> mimeService = do_GetService("@mozilla.org/mime;1");
  if (mimeService) {
    if (fileName.IsEmpty()) {
      nsCOMPtr<nsIURL> url = do_QueryInterface(aURI);
      // Try to extract the file name from the url and use that as a first
      // pass as the leaf name of our temp file...
      if (url) {
        nsAutoCString leafName;
        url->GetFileName(leafName);
        if (!leafName.IsEmpty()) {
          if (NS_FAILED(UnescapeFragment(leafName, url, fileName))) {
            CopyUTF8toUTF16(leafName, fileName);  // use escaped name instead
            fileName.Trim(".");
          }
        }

        // Only get the extension from the URL if allowed, or if this
        // is a binary type in which case the type might not be valid
        // anyway.
        if (aAllowURLExtension || isBinaryType || urlIsFile) {
          url->GetFileExtension(extension);
        }
      }
    } else {
      // Determine the current extension for the filename.
      int32_t dotidx = fileName.RFind(u".");
      if (dotidx != -1) {
        CopyUTF16toUTF8(Substring(fileName, dotidx + 1), extension);
      }
    }

    if (aFlags & VALIDATE_GUESS_FROM_EXTENSION) {
      nsAutoCString mimeType;
      if (!extension.IsEmpty()) {
        mimeService->GetFromTypeAndExtension(EmptyCString(), extension,
                                             getter_AddRefs(mimeInfo));
        if (mimeInfo) {
          mimeInfo->GetMIMEType(mimeType);
        }
      }

      if (mimeType.IsEmpty()) {
        // Extension lookup gave us no useful match, so use octet-stream
        // instead.
        mimeService->GetFromTypeAndExtension(
            nsLiteralCString(APPLICATION_OCTET_STREAM), extension,
            getter_AddRefs(mimeInfo));
      }
    } else if (!aMimeType.IsEmpty()) {
      // If this is a binary type, include the extension as a hint to get
      // the mime info. For other types, the mime type itself should be
      // sufficient.
      // Unfortunately, on Windows, the mimetype is usually insufficient.
      // Compensate at least on `file` URLs by trusting the extension -
      // that's likely what we used to get the mimetype in the first place.
      // The special case for application/ogg is because that type could
      // actually be used for a video which can better be determined by the
      // extension. This is tested by browser_save_video.js.
      bool useExtension =
          isBinaryType || urlIsFile || aMimeType.EqualsLiteral(APPLICATION_OGG);
      mimeService->GetFromTypeAndExtension(
          aMimeType, useExtension ? extension : EmptyCString(),
          getter_AddRefs(mimeInfo));
      if (mimeInfo) {
        // But if no primary extension was returned, this mime type is probably
        // an unknown type. Look it up again but this time supply the extension.
        nsAutoCString primaryExtension;
        mimeInfo->GetPrimaryExtension(primaryExtension);
        if (primaryExtension.IsEmpty()) {
          mimeService->GetFromTypeAndExtension(aMimeType, extension,
                                               getter_AddRefs(mimeInfo));
        }
      }
    }
  }

  // If an empty filename is allowed, then return early. It will be saved
  // using the filename of the temporary file that was created for the download.
  if (aFlags & VALIDATE_ALLOW_EMPTY && fileName.IsEmpty()) {
    aFileName.Truncate();
    return mimeInfo.forget();
  }

  // This section modifies the extension on the filename if it isn't valid for
  // the given content type.
  if (mimeInfo) {
    bool isValidExtension;
    if (extension.IsEmpty() ||
        NS_FAILED(mimeInfo->ExtensionExists(extension, &isValidExtension)) ||
        !isValidExtension) {
      // Skip these checks for text and binary, so we don't append the unneeded
      // .txt or other extension.
      if (aMimeType.EqualsLiteral(TEXT_PLAIN) || isBinaryType) {
        extension.Truncate();
      } else {
        nsAutoCString originalExtension(extension);
        // If an original url was supplied, see if it has a valid extension.
        bool useOldExtension = false;
        if (aOriginalURI) {
          nsCOMPtr<nsIURL> originalURL(do_QueryInterface(aOriginalURI));
          if (originalURL) {
            nsAutoCString uriExtension;
            originalURL->GetFileExtension(uriExtension);
            if (!uriExtension.IsEmpty()) {
              mimeInfo->ExtensionExists(uriExtension, &useOldExtension);
              if (useOldExtension) {
                extension = uriExtension;
              }
            }
          }
        }

        if (!useOldExtension) {
          // If the filename doesn't have a valid extension, or we don't know
          // the extension, try to use the primary extension for the type. If we
          // don't know the primary extension for the type, just continue with
          // the existing extension, or leave the filename with no extension.
          nsAutoCString primaryExtension;
          mimeInfo->GetPrimaryExtension(primaryExtension);
          if (!primaryExtension.IsEmpty()) {
            extension = primaryExtension;
          }
        }

        // If an suitable extension was found, we will append to or replace the
        // existing extension.
        if (!extension.IsEmpty()) {
          ModifyExtensionType modify = ShouldModifyExtension(
              mimeInfo, aFlags & VALIDATE_FORCE_APPEND_EXTENSION,
              originalExtension);
          if (modify == ModifyExtension_Replace) {
            int32_t dotidx = fileName.RFind(u".");
            if (dotidx != -1) {
              // Remove the existing extension and replace it.
              fileName.Truncate(dotidx);
            }
          }

          // Otherwise, just append the proper extension to the end of the
          // filename, adding to the invalid extension that might already be
          // there.
          if (modify != ModifyExtension_Ignore) {
            fileName.AppendLiteral(".");
            fileName.Append(NS_ConvertUTF8toUTF16(extension));
          }
        }
      }
    }
  }

  CheckDefaultFileName(fileName, aFlags);

  // Make the filename safe for the filesystem.
  SanitizeFileName(fileName, aFlags);

  aFileName = fileName;
  return mimeInfo.forget();
}

void nsExternalHelperAppService::CheckDefaultFileName(nsAString& aFileName,
                                                      uint32_t aFlags) {
  // If no filename is present, use a default filename.
  if (!(aFlags & VALIDATE_NO_DEFAULT_FILENAME) &&
      (aFileName.Length() == 0 || aFileName.RFind(u".") == 0)) {
    nsCOMPtr<nsIStringBundleService> stringService =
        mozilla::components::StringBundle::Service();
    if (stringService) {
      nsCOMPtr<nsIStringBundle> bundle;
      if (NS_SUCCEEDED(stringService->CreateBundle(
              "chrome://global/locale/contentAreaCommands.properties",
              getter_AddRefs(bundle)))) {
        nsAutoString defaultFileName;
        bundle->GetStringFromName("UntitledSaveFileName", defaultFileName);
        // Append any existing extension to the default filename.
        aFileName = defaultFileName + aFileName;
      }
    }

    // Use 'Untitled' as a last resort.
    if (!aFileName.Length()) {
      aFileName.AssignLiteral("Untitled");
    }
  }
}

void nsExternalHelperAppService::SanitizeFileName(nsAString& aFileName,
                                                  uint32_t aFlags) {
  nsAutoString fileName(aFileName);

  // Replace known invalid characters.
  fileName.ReplaceChar(u"" KNOWN_PATH_SEPARATORS FILE_ILLEGAL_CHARACTERS "%",
                       u'_');
  fileName.StripChar(char16_t(0));

  const char16_t *startStr, *endStr;
  fileName.BeginReading(startStr);
  fileName.EndReading(endStr);

  // True if multiple consecutive whitespace characters should
  // be replaced by single space ' '.
  bool collapseWhitespace = !(aFlags & VALIDATE_DONT_COLLAPSE_WHITESPACE);

  // The maximum filename length differs based on the platform:
  //  Windows (FAT/NTFS) stores filenames as a maximum of 255 UTF-16 code units.
  //  Mac (APFS) stores filenames with a maximum 255 of UTF-8 code units.
  //  Linux (ext3/ext4...) stores filenames with a maximum 255 bytes.
  // So here we just use the maximum of 255 bytes.
  // 0 means don't truncate at a maximum size.
  const uint32_t maxBytes =
      (aFlags & VALIDATE_DONT_TRUNCATE) ? 0 : kDefaultMaxFileNameLength;

  // True if the last character added was whitespace.
  bool lastWasWhitespace = false;

  // Length of the filename that fits into the maximum size excluding the
  // extension and period.
  int32_t longFileNameEnd = -1;

  // Index of the last character added that was not a character that can be
  // trimmed off of the end of the string. Trimmable characters are whitespace,
  // periods and the vowel separator u'\u180e'. If all the characters after this
  // point are trimmable characters, truncate the string to this point after
  // iterating over the filename.
  int32_t lastNonTrimmable = -1;

  // The number of bytes that the string would occupy if encoded in UTF-8.
  uint32_t bytesLength = 0;

  // The length of the extension in bytes.
  uint32_t extensionBytesLength = 0;

  // This algorithm iterates over each character in the string and appends it
  // or a replacement character if needed to outFileName.
  nsAutoString outFileName;
  while (startStr < endStr) {
    bool err = false;
    char32_t nextChar = UTF16CharEnumerator::NextChar(&startStr, endStr, &err);
    if (err) {
      break;
    }

    // nulls are already stripped out above.
    MOZ_ASSERT(nextChar != char16_t(0));

    auto unicodeCategory = unicode::GetGeneralCategory(nextChar);
    if (unicodeCategory == HB_UNICODE_GENERAL_CATEGORY_CONTROL ||
        unicodeCategory == HB_UNICODE_GENERAL_CATEGORY_LINE_SEPARATOR ||
        unicodeCategory == HB_UNICODE_GENERAL_CATEGORY_PARAGRAPH_SEPARATOR) {
      // Skip over any control characters and separators.
      continue;
    }

    if (unicodeCategory == HB_UNICODE_GENERAL_CATEGORY_SPACE_SEPARATOR ||
        nextChar == u'\ufeff') {
      // Trim out any whitespace characters at the beginning of the filename,
      // and only add whitespace in the middle of the filename if the last
      // character was not whitespace or if we are not collapsing whitespace.
      if (!outFileName.IsEmpty() &&
          (!lastWasWhitespace || !collapseWhitespace)) {
        // Allow the ideographic space if it is present, otherwise replace with
        // ' '.
        if (nextChar != u'\u3000') {
          nextChar = ' ';
        }
        lastWasWhitespace = true;
      } else {
        lastWasWhitespace = true;
        continue;
      }
    } else {
      lastWasWhitespace = false;
      if (nextChar == '.' || nextChar == u'\u180e') {
        // Don't add any periods or vowel separators at the beginning of the
        // string. Note also that lastNonTrimmable is not adjusted in this
        // case, because periods and vowel separators are included in the
        // set of characters to trim at the end of the filename.
        if (outFileName.IsEmpty()) {
          continue;
        }
      } else {
        if (unicodeCategory == HB_UNICODE_GENERAL_CATEGORY_FORMAT) {
          // Replace formatting characters with an underscore.
          nextChar = '_';
        }

        // Don't truncate surrogate pairs in the middle.
        lastNonTrimmable =
            int32_t(outFileName.Length()) +
            (NS_IS_HIGH_SURROGATE(H_SURROGATE(nextChar)) ? 2 : 1);
      }
    }

    if (maxBytes) {
      // UTF16CharEnumerator already converts surrogate pairs, so we can use
      // a simple computation of byte length here.
      uint32_t charBytesLength = nextChar < 0x80      ? 1
                                 : nextChar < 0x800   ? 2
                                 : nextChar < 0x10000 ? 3
                                                      : 4;
      bytesLength += charBytesLength;
      if (bytesLength > maxBytes) {
        if (longFileNameEnd == -1) {
          longFileNameEnd = int32_t(outFileName.Length());
        }
      }

      // If we encounter a period, it could be the start of an extension, so
      // start counting the number of bytes in the extension. If another period
      // is found, start again since we want to use the last extension found.
      if (nextChar == u'.') {
        extensionBytesLength = 1;  // 1 byte for the period.
      } else if (extensionBytesLength) {
        extensionBytesLength += charBytesLength;
      }
    }

    AppendUCS4ToUTF16(nextChar, outFileName);
  }

  // If the filename is longer than the maximum allowed filename size,
  // truncate it, but preserve the desired extension that is currently
  // on the filename.
  if (bytesLength > maxBytes && !outFileName.IsEmpty()) {
    // Get the sanitized extension from the filename without the dot.
    nsAutoString extension;
    int32_t dotidx = outFileName.RFind(u".");
    if (dotidx != -1) {
      extension = Substring(outFileName, dotidx + 1);
    }

    // There are two ways in which the filename should be truncated:
    //   - If the filename was too long, truncate the name at the length
    //     of the filename.
    //     This position is indicated by longFileNameEnd.
    //   - lastNonTrimmable will indicate the last character that was not
    //     whitespace, a period, or a vowel separator at the end of the
    //     the string, so the string should be truncated there as well.
    // If both apply, use the earliest position.
    if (lastNonTrimmable >= 0) {
      // Subtract off the amount for the extension and the period.
      // Note that the extension length is in bytes but longFileNameEnd is in
      // characters, but if they don't match, it just means we crop off
      // more than is necessary. This is OK since it is better than cropping
      // off too little.
      longFileNameEnd -= extensionBytesLength;
      if (longFileNameEnd <= 0) {
        // This is extremely unlikely, but if the extension is larger than the
        // maximum size, just get rid of it. In this case, the extension
        // wouldn't have been an ordinary one we would want to preserve (such
        // as .html or .png) so just truncate off the file wherever the first
        // period appears.
        int32_t dotidx = outFileName.Find(u".");
        outFileName.Truncate(dotidx > 0 ? dotidx : 1);
      } else {
        outFileName.Truncate(std::min(longFileNameEnd, lastNonTrimmable));

        // Now that the filename has been truncated, re-append the extension
        // again.
        if (!extension.IsEmpty()) {
          if (outFileName.Last() != '.') {
            outFileName.AppendLiteral(".");
          }

          outFileName.Append(extension);
        }
      }
    }
  } else if (lastNonTrimmable >= 0) {
    // Otherwise, the filename wasn't too long, so just trim off the
    // extra whitespace and periods at the end.
    outFileName.Truncate(lastNonTrimmable);
  }

  if (!(aFlags & VALIDATE_ALLOW_DIRECTORY_NAMES)) {
    nsAutoString extension;
    int32_t dotidx = outFileName.RFind(u".");
    if (dotidx != -1) {
      extension = Substring(outFileName, dotidx + 1);
      extension.StripWhitespace();
      outFileName = Substring(outFileName, 0, dotidx + 1) + extension;
    }
  }

#ifdef XP_WIN
  if (nsLocalFile::CheckForReservedFileName(outFileName)) {
    outFileName.Truncate();
    CheckDefaultFileName(outFileName, aFlags);
  }

#endif

  if (!(aFlags & VALIDATE_ALLOW_INVALID_FILENAMES)) {
    // If the extension is one these types, replace it with .download, as these
    // types of files can have significance on Windows or Linux.
    // This happens for any file, not just those with the shortcut mime type.
    if (StringEndsWith(outFileName, u".lnk"_ns,
                       nsCaseInsensitiveStringComparator) ||
        StringEndsWith(outFileName, u".local"_ns,
                       nsCaseInsensitiveStringComparator) ||
        StringEndsWith(outFileName, u".url"_ns,
                       nsCaseInsensitiveStringComparator) ||
        StringEndsWith(outFileName, u".scf"_ns,
                       nsCaseInsensitiveStringComparator) ||
        StringEndsWith(outFileName, u".desktop"_ns,
                       nsCaseInsensitiveStringComparator)) {
      outFileName.AppendLiteral(".download");
    }
  }

  aFileName = outFileName;
}

nsExternalHelperAppService::ModifyExtensionType
nsExternalHelperAppService::ShouldModifyExtension(nsIMIMEInfo* aMimeInfo,
                                                  bool aForceAppend,
                                                  const nsCString& aFileExt) {
  nsAutoCString MIMEType;
  if (!aMimeInfo || NS_FAILED(aMimeInfo->GetMIMEType(MIMEType))) {
    return ModifyExtension_Append;
  }

  // Special cases where we want to keep file extensions if they're common
  // for a given MIME type to avoid surprising users with a changed extension.
  static constexpr std::pair<nsLiteralCString, nsLiteralCString>
      ignoreMimeExtPairs[] = {
          {"video/3gpp"_ns, "mp4"_ns},   // bug 1749294
          {"audio/x-wav"_ns, "mp2"_ns},  // bug 1805365
      };

  nsAutoCString fileExtLowerCase(aFileExt);
  ToLowerCase(fileExtLowerCase);
  for (const auto& [mime, ext] : ignoreMimeExtPairs) {
    if (MIMEType.Equals(mime) && fileExtLowerCase.Equals(ext)) {
      return ModifyExtension_Ignore;
    }
  }

  // Determine whether the extensions should be appended or replaced depending
  // on the content type.
  bool canForce = StringBeginsWith(MIMEType, "image/"_ns) ||
                  StringBeginsWith(MIMEType, "audio/"_ns) ||
                  StringBeginsWith(MIMEType, "video/"_ns) || aFileExt.IsEmpty();

  if (!canForce) {
    for (const char* mime : forcedExtensionMimetypes) {
      if (MIMEType.Equals(mime)) {
        if (!StaticPrefs::browser_download_sanitize_non_media_extensions()) {
          return ModifyExtension_Ignore;
        }
        canForce = true;
        break;
      }
    }

    if (!canForce) {
      return aForceAppend ? ModifyExtension_Append : ModifyExtension_Ignore;
    }
  }

  // If we get here, we know for sure the mimetype allows us to modify the
  // existing extension, if it's wrong. Return whether we should replace it
  // or append it.
  bool knownExtension = false;
  // Note that aFileExt is either empty or consists of an extension
  // excluding the dot.
  if (aFileExt.IsEmpty() ||
      (NS_SUCCEEDED(aMimeInfo->ExtensionExists(aFileExt, &knownExtension)) &&
       !knownExtension)) {
    return ModifyExtension_Replace;
  }

  return ModifyExtension_Append;
}
