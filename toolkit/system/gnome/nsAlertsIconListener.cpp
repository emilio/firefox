/* -*- Mode: C++; tab-width: 2; indent-tabs-mode:nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsAlertsIconListener.h"
#include "imgIContainer.h"
#include "imgIRequest.h"
#include "nsServiceManagerUtils.h"
#include "nsSystemAlertsService.h"
#include "nsIAlertsService.h"
#include "nsICancelable.h"
#include "nsImageToPixbuf.h"
#include "nsIStringBundle.h"
#include "nsIObserverService.h"
#include "nsCRT.h"
#include "mozilla/XREAppData.h"
#include "mozilla/GRefPtr.h"
#include "mozilla/GUniquePtr.h"
#include "mozilla/UniquePtrExtensions.h"

#include <dlfcn.h>
#include <gdk/gdk.h>

using namespace mozilla;
extern const StaticXREAppData* gAppData;

static bool gHasActions = false;
static bool gHasCaps = false;
static bool gBodySupportsMarkup = false;

static constexpr nsLiteralCString kActionSuffix = "-moz"_ns;

void* nsAlertsIconListener::libNotifyHandle = nullptr;
bool nsAlertsIconListener::libNotifyNotAvail = false;
nsAlertsIconListener::notify_is_initted_t
    nsAlertsIconListener::notify_is_initted = nullptr;
nsAlertsIconListener::notify_init_t nsAlertsIconListener::notify_init = nullptr;
nsAlertsIconListener::notify_get_server_caps_t
    nsAlertsIconListener::notify_get_server_caps = nullptr;
nsAlertsIconListener::notify_notification_new_t
    nsAlertsIconListener::notify_notification_new = nullptr;
nsAlertsIconListener::notify_notification_show_t
    nsAlertsIconListener::notify_notification_show = nullptr;
nsAlertsIconListener::notify_notification_set_icon_from_pixbuf_t
    nsAlertsIconListener::notify_notification_set_icon_from_pixbuf = nullptr;
nsAlertsIconListener::notify_notification_add_action_t
    nsAlertsIconListener::notify_notification_add_action = nullptr;
nsAlertsIconListener::notify_notification_close_t
    nsAlertsIconListener::notify_notification_close = nullptr;
nsAlertsIconListener::notify_notification_set_hint_t
    nsAlertsIconListener::notify_notification_set_hint = nullptr;
nsAlertsIconListener::notify_notification_set_timeout_t
    nsAlertsIconListener::notify_notification_set_timeout = nullptr;

static void notify_action_cb(NotifyNotification* notification, gchar* action,
                             gpointer user_data) {
  nsAlertsIconListener* alert = static_cast<nsAlertsIconListener*>(user_data);
  alert->SendCallback();
}

static void notify_nondefault_action_cb(NotifyNotification* notification,
                                        gchar* action, gpointer user_data) {
  nsAlertsIconListener* alert = static_cast<nsAlertsIconListener*>(user_data);
  nsCString actionName(action);

  // Trim the suffix
  actionName.Truncate(actionName.Length() - kActionSuffix.Length());
  alert->SendActionCallback(NS_ConvertUTF8toUTF16(actionName));
}

static void notify_closed_marshal(GClosure* closure, GValue* return_value,
                                  guint n_param_values,
                                  const GValue* param_values,
                                  gpointer invocation_hint,
                                  gpointer marshal_data) {
  MOZ_ASSERT(n_param_values >= 1, "No object in params");

  nsAlertsIconListener* alert =
      static_cast<nsAlertsIconListener*>(closure->data);
  alert->SendClosed();
  NS_RELEASE(alert);
}

static already_AddRefed<GdkPixbuf> GetPixbufFromImgRequest(
    imgIRequest* aRequest) {
  nsCOMPtr<imgIContainer> image;
  nsresult rv = aRequest->GetImage(getter_AddRefs(image));
  if (NS_FAILED(rv)) {
    return nullptr;
  }

  int32_t width = 0, height = 0;
  const int32_t kBytesPerPixel = 4;
  // DBUS_MAXIMUM_ARRAY_LENGTH is 64M, there is 60 bytes overhead
  // for the hints array with only the image payload, 256 is used to give
  // some breathing room.
  const int32_t kMaxImageBytes = 64 * 1024 * 1024 - 256;
  image->GetWidth(&width);
  image->GetHeight(&height);
  if (width * height * kBytesPerPixel > kMaxImageBytes) {
    // The image won't fit in a dbus array
    return nullptr;
  }

  return nsImageToPixbuf::ImageToPixbuf(image);
}

NS_IMPL_ISUPPORTS(nsAlertsIconListener, nsIAlertNotificationImageListener)

nsAlertsIconListener::nsAlertsIconListener(
    nsSystemAlertsService* aBackend, nsIAlertNotification* aAlertNotification,
    const nsAString& aAlertName)
    : mAlertName(aAlertName),
      mBackend(aBackend),
      mAlertNotification(aAlertNotification) {
  if (!libNotifyHandle && !libNotifyNotAvail) {
    libNotifyHandle = dlopen("libnotify.so.4", RTLD_LAZY);
    if (!libNotifyHandle) {
      libNotifyHandle = dlopen("libnotify.so.1", RTLD_LAZY);
      if (!libNotifyHandle) {
        libNotifyNotAvail = true;
        return;
      }
    }

    notify_is_initted =
        (notify_is_initted_t)dlsym(libNotifyHandle, "notify_is_initted");
    notify_init = (notify_init_t)dlsym(libNotifyHandle, "notify_init");
    notify_get_server_caps = (notify_get_server_caps_t)dlsym(
        libNotifyHandle, "notify_get_server_caps");
    notify_notification_new = (notify_notification_new_t)dlsym(
        libNotifyHandle, "notify_notification_new");
    notify_notification_show = (notify_notification_show_t)dlsym(
        libNotifyHandle, "notify_notification_show");
    notify_notification_set_icon_from_pixbuf =
        (notify_notification_set_icon_from_pixbuf_t)dlsym(
            libNotifyHandle, "notify_notification_set_icon_from_pixbuf");
    notify_notification_add_action = (notify_notification_add_action_t)dlsym(
        libNotifyHandle, "notify_notification_add_action");
    notify_notification_close = (notify_notification_close_t)dlsym(
        libNotifyHandle, "notify_notification_close");
    notify_notification_set_hint = (notify_notification_set_hint_t)dlsym(
        libNotifyHandle, "notify_notification_set_hint");
    notify_notification_set_timeout = (notify_notification_set_timeout_t)dlsym(
        libNotifyHandle, "notify_notification_set_timeout");
    if (!notify_is_initted || !notify_init || !notify_get_server_caps ||
        !notify_notification_new || !notify_notification_show ||
        !notify_notification_set_icon_from_pixbuf ||
        !notify_notification_add_action || !notify_notification_close) {
      dlclose(libNotifyHandle);
      libNotifyHandle = nullptr;
    }
  }
}

nsAlertsIconListener::~nsAlertsIconListener() {
  mBackend->RemoveListener(mAlertName, this);
  // Don't dlclose libnotify as it uses atexit().
}

NS_IMETHODIMP
nsAlertsIconListener::OnImageMissing(nsISupports*) {
  // This notification doesn't have an image, or there was an error getting
  // the image. Show the notification without an icon.
  return ShowAlert(nullptr);
}

NS_IMETHODIMP
nsAlertsIconListener::OnImageReady(nsISupports*, imgIRequest* aRequest) {
  RefPtr<GdkPixbuf> imagePixbuf = GetPixbufFromImgRequest(aRequest);
  ShowAlert(imagePixbuf);
  return NS_OK;
}

nsresult nsAlertsIconListener::ShowAlert(GdkPixbuf* aPixbuf) {
  if (!mBackend->IsActiveListener(mAlertName, this)) return NS_OK;

  mNotification = notify_notification_new(mAlertTitle.get(), mAlertText.get(),
                                          nullptr, nullptr);

  if (!mNotification) return NS_ERROR_OUT_OF_MEMORY;

  if (aPixbuf) notify_notification_set_icon_from_pixbuf(mNotification, aPixbuf);

  NS_ADDREF(this);
  if (mAlertHasAction) {
    // What we put as the label doesn't matter here, if the action
    // string is "default" then that makes the entire bubble clickable
    // rather than creating a button.
    notify_notification_add_action(mNotification, "default", "Activate",
                                   notify_action_cb, this, nullptr);
  }

  for (const RefPtr<nsIAlertAction>& action : mActions) {
    nsAutoString actionName;
    MOZ_TRY(action->GetAction(actionName));
    nsAutoCString actionNameUTF8;
    CopyUTF16toUTF8(actionName, actionNameUTF8);
    // Add suffix to prevent potential collision with keywords like "default"
    actionNameUTF8.Append(kActionSuffix);

    nsAutoString actionTitle;
    MOZ_TRY(action->GetTitle(actionTitle));
    nsAutoCString actionTitleUTF8;
    CopyUTF16toUTF8(actionTitle, actionTitleUTF8);

    notify_notification_add_action(mNotification, actionNameUTF8.get(),
                                   actionTitleUTF8.get(),
                                   notify_nondefault_action_cb, this, nullptr);
  }

  if (notify_notification_set_hint) {
    notify_notification_set_hint(mNotification, "suppress-sound",
                                 g_variant_new_boolean(mAlertIsSilent));

    // If MOZ_DESKTOP_FILE_NAME variable is set, use it as the application id,
    // otherwise use gAppData->name
    if (getenv("MOZ_DESKTOP_FILE_NAME")) {
      // Send the desktop name to identify the application
      // The desktop-entry is the part before the .desktop
      notify_notification_set_hint(
          mNotification, "desktop-entry",
          g_variant_new("s", getenv("MOZ_DESKTOP_FILE_NAME")));
    } else {
      notify_notification_set_hint(mNotification, "desktop-entry",
                                   g_variant_new("s", gAppData->remotingName));
    }
  }

  if (notify_notification_set_timeout && mAlertRequiresInteraction) {
    constexpr gint kNotifyExpiresNever = 0;
    notify_notification_set_timeout(mNotification, kNotifyExpiresNever);
  }

  // Fedora 10 calls NotifyNotification "closed" signal handlers with a
  // different signature, so a marshaller is used instead of a C callback to
  // get the user_data (this) in a parseable format.  |closure| is created
  // with a floating reference, which gets sunk by g_signal_connect_closure().
  GClosure* closure = g_closure_new_simple(sizeof(GClosure), this);
  g_closure_set_marshal(closure, notify_closed_marshal);
  mClosureHandler =
      g_signal_connect_closure(mNotification, "closed", closure, FALSE);
  GUniquePtr<GError> error;
  if (!notify_notification_show(mNotification, getter_Transfers(error))) {
    NS_WARNING(error->message);
    return NS_ERROR_FAILURE;
  }

  if (mAlertListener) {
    mAlertListener->Observe(nullptr, "alertshow", mAlertCookie.get());
  }

  return NS_OK;
}

void nsAlertsIconListener::SendCallback() {
  if (mAlertListener) {
    mAlertListener->Observe(nullptr, "alertclickcallback", mAlertCookie.get());
  }
}

void nsAlertsIconListener::SendActionCallback(const nsAString& aActionName) {
  if (mAlertListener) {
    nsCOMPtr<nsIAlertAction> alertAction;
    mAlertNotification->GetAction(aActionName, getter_AddRefs(alertAction));
    mAlertListener->Observe(alertAction, "alertclickcallback",
                            mAlertCookie.get());
  }
}

void nsAlertsIconListener::SendClosed() {
  if (mNotification) {
    g_object_unref(mNotification);
    mNotification = nullptr;
  }
  NotifyFinished();
}

void nsAlertsIconListener::Disconnect() {
  if (!mNotification) {
    // Already disconnected
    return;
  }

  // We need to close any open notifications upon application exit, otherwise
  // we will leak since libnotify holds a ref for us.
  g_signal_handler_disconnect(mNotification, mClosureHandler);
  g_object_unref(mNotification);
  mNotification = nullptr;
  Release();  // equivalent to NS_RELEASE(this)
}

nsresult nsAlertsIconListener::Close() {
  if (mIconRequest) {
    mIconRequest->Cancel(NS_BINDING_ABORTED);
    mIconRequest = nullptr;
  }

  NotifyFinished();

  if (!mNotification) {
    return NS_OK;
  }

  GUniquePtr<GError> error;
  if (!notify_notification_close(mNotification, getter_Transfers(error))) {
    NS_WARNING(error->message);
    return NS_ERROR_FAILURE;
  }

  return NS_OK;
}

nsresult nsAlertsIconListener::InitAlertAsync(nsIAlertNotification* aAlert,
                                              nsIObserver* aAlertListener) {
  if (!libNotifyHandle) return NS_ERROR_FAILURE;

  if (!notify_is_initted()) {
    // Give the name of this application to libnotify
    nsCOMPtr<nsIStringBundleService> bundleService =
        do_GetService(NS_STRINGBUNDLE_CONTRACTID);

    nsAutoCString appShortName;
    if (bundleService) {
      nsCOMPtr<nsIStringBundle> bundle;
      bundleService->CreateBundle("chrome://branding/locale/brand.properties",
                                  getter_AddRefs(bundle));
      nsAutoString appName;

      if (bundle) {
        bundle->GetStringFromName("brandShortName", appName);
        CopyUTF16toUTF8(appName, appShortName);
      } else {
        NS_WARNING(
            "brand.properties not present, using default application name");
        appShortName.AssignLiteral("Mozilla");
      }
    } else {
      appShortName.AssignLiteral("Mozilla");
    }

    if (!notify_init(appShortName.get())) return NS_ERROR_FAILURE;

    GList* server_caps = notify_get_server_caps();
    if (server_caps) {
      gHasCaps = true;
      for (GList* cap = server_caps; cap != nullptr; cap = cap->next) {
        if (!strcmp((char*)cap->data, "actions")) {
          gHasActions = true;
          continue;
        }
        if (!strcmp((char*)cap->data, "body-markup")) {
          gBodySupportsMarkup = true;
          continue;
        }
      }
      g_list_foreach(server_caps, (GFunc)g_free, nullptr);
      g_list_free(server_caps);
    }
  }

  if (!gHasCaps) {
    // if notify_get_server_caps() failed above we need to assume
    // there is no notification-server to display anything
    return NS_ERROR_FAILURE;
  }

  nsresult rv = aAlert->GetTextClickable(&mAlertHasAction);
  NS_ENSURE_SUCCESS(rv, rv);
  if (!gHasActions && mAlertHasAction) {
    return NS_ERROR_FAILURE;  // No good, fallback to XUL
  }

  MOZ_TRY(aAlert->GetActions(mActions));
  if (!gHasActions && mActions.Length() > 0) {
    return NS_ERROR_FAILURE;  // No good, fallback to XUL
  }

  rv = aAlert->GetSilent(&mAlertIsSilent);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = aAlert->GetRequireInteraction(&mAlertRequiresInteraction);
  NS_ENSURE_SUCCESS(rv, rv);

  nsAutoString title;
  rv = aAlert->GetTitle(title);
  NS_ENSURE_SUCCESS(rv, rv);
  // Workaround for a libnotify bug - blank titles aren't dealt with
  // properly so we use a space
  if (title.IsEmpty()) {
    mAlertTitle = " "_ns;
  } else {
    CopyUTF16toUTF8(title, mAlertTitle);
  }

  nsAutoString text;
  rv = aAlert->GetText(text);
  NS_ENSURE_SUCCESS(rv, rv);
  CopyUTF16toUTF8(text, mAlertText);
  if (gBodySupportsMarkup) {
    NS_ENSURE_TRUE(
        mAlertText.ReplaceSubstring(u8"&"_ns, u8"&amp;"_ns, mozilla::fallible),
        NS_ERROR_FAILURE);
    NS_ENSURE_TRUE(
        mAlertText.ReplaceSubstring(u8"<"_ns, u8"&lt;"_ns, mozilla::fallible),
        NS_ERROR_FAILURE);
    NS_ENSURE_TRUE(
        mAlertText.ReplaceSubstring(u8">"_ns, u8"&gt;"_ns, mozilla::fallible),
        NS_ERROR_FAILURE);
  }

  mAlertListener = aAlertListener;

  rv = aAlert->GetCookie(mAlertCookie);
  NS_ENSURE_SUCCESS(rv, rv);

  return aAlert->LoadImage(/* aTimeout = */ 0, this, /* aUserData = */ nullptr,
                           getter_AddRefs(mIconRequest));
}

void nsAlertsIconListener::NotifyFinished() {
  if (nsCOMPtr<nsIObserver> alertListener = mAlertListener.forget()) {
    alertListener->Observe(nullptr, "alertfinished", mAlertCookie.get());
  }
}
