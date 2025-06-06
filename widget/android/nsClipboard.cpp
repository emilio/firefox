/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/java/ClipboardWrappers.h"
#include "mozilla/java/GeckoAppShellWrappers.h"
#include "nsClipboard.h"
#include "nsISupportsPrimitives.h"
#include "nsCOMPtr.h"
#include "nsComponentManagerUtils.h"
#include "nsMemory.h"
#include "nsStringStream.h"
#include "nsPrimitiveHelpers.h"

using namespace mozilla;

NS_IMPL_ISUPPORTS_INHERITED0(nsClipboard, nsBaseClipboard)

/* The Android clipboard only supports text and doesn't support mime types
 * so we assume all clipboard data is text/plain for now. Documentation
 * indicates that support for other data types is planned for future
 * releases.
 */

nsClipboard::nsClipboard()
    : nsBaseClipboard(mozilla::dom::ClipboardCapabilities(
          false /* supportsSelectionClipboard */,
          false /* supportsFindClipboard */,
          false /* supportsSelectionCache */)) {
  java::Clipboard::StartTrackingClipboardData(
      java::GeckoAppShell::GetApplicationContext());
}

nsClipboard::~nsClipboard() {
  java::Clipboard::StopTrackingClipboardData(
      java::GeckoAppShell::GetApplicationContext());
}

// static
nsresult nsClipboard::GetTextFromTransferable(nsITransferable* aTransferable,
                                              nsString& aText,
                                              nsString& aHTML) {
  nsTArray<nsCString> flavors;
  nsresult rv = aTransferable->FlavorsTransferableCanImport(flavors);
  if (NS_FAILED(rv)) {
    return rv;
  }

  for (auto& flavorStr : flavors) {
    if (flavorStr.EqualsLiteral(kTextMime)) {
      nsCOMPtr<nsISupports> item;
      nsresult rv =
          aTransferable->GetTransferData(kTextMime, getter_AddRefs(item));
      if (NS_WARN_IF(NS_FAILED(rv))) {
        continue;
      }
      nsCOMPtr<nsISupportsString> supportsString = do_QueryInterface(item);
      if (supportsString) {
        supportsString->GetData(aText);
      }
    } else if (flavorStr.EqualsLiteral(kHTMLMime)) {
      nsCOMPtr<nsISupports> item;
      nsresult rv =
          aTransferable->GetTransferData(kHTMLMime, getter_AddRefs(item));
      if (NS_WARN_IF(NS_FAILED(rv))) {
        continue;
      }
      nsCOMPtr<nsISupportsString> supportsString = do_QueryInterface(item);
      if (supportsString) {
        supportsString->GetData(aHTML);
      }
    }
  }
  return NS_OK;
}

NS_IMETHODIMP
nsClipboard::SetNativeClipboardData(nsITransferable* aTransferable,
                                    ClipboardType aWhichClipboard) {
  MOZ_DIAGNOSTIC_ASSERT(aTransferable);
  MOZ_DIAGNOSTIC_ASSERT(
      nsIClipboard::IsClipboardTypeSupported(aWhichClipboard));

  if (!jni::IsAvailable()) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  nsString text;
  nsString html;
  nsresult rv = GetTextFromTransferable(aTransferable, text, html);
  if (NS_FAILED(rv)) {
    return rv;
  }

  bool isPrivate = aTransferable->GetIsPrivateData();

  if (!html.IsEmpty() &&
      java::Clipboard::SetHTML(java::GeckoAppShell::GetApplicationContext(),
                               text, html, isPrivate)) {
    return NS_OK;
  }
  if (!text.IsEmpty() &&
      java::Clipboard::SetText(java::GeckoAppShell::GetApplicationContext(),
                               text, isPrivate)) {
    return NS_OK;
  }

  return NS_ERROR_FAILURE;
}

mozilla::Result<nsCOMPtr<nsISupports>, nsresult>
nsClipboard::GetNativeClipboardData(const nsACString& aFlavor,
                                    ClipboardType aWhichClipboard) {
  MOZ_DIAGNOSTIC_ASSERT(
      nsIClipboard::IsClipboardTypeSupported(aWhichClipboard));

  if (!jni::IsAvailable()) {
    return Err(NS_ERROR_NOT_AVAILABLE);
  }

  if (aFlavor.EqualsLiteral(kTextMime) || aFlavor.EqualsLiteral(kHTMLMime)) {
    auto text = java::Clipboard::GetTextData(
        java::GeckoAppShell::GetApplicationContext(), aFlavor);
    if (!text) {
      return nsCOMPtr<nsISupports>{};
    }
    nsString buffer = text->ToString();
    if (buffer.IsEmpty()) {
      return nsCOMPtr<nsISupports>{};
    }
    nsCOMPtr<nsISupports> wrapper;
    nsPrimitiveHelpers::CreatePrimitiveForData(
        aFlavor, buffer.get(), buffer.Length() * 2, getter_AddRefs(wrapper));
    return std::move(wrapper);
  }

  mozilla::jni::ByteArray::LocalRef bytes;
  nsresult rv = java::Clipboard::GetRawData(aFlavor, &bytes);
  if (NS_FAILED(rv) || !bytes) {
    return nsCOMPtr<nsISupports>{};
  }

  nsCOMPtr<nsIInputStream> byteStream;
  rv = NS_NewByteInputStream(getter_AddRefs(byteStream),
                             mozilla::Span(reinterpret_cast<const char*>(
                                               bytes->GetElements().Elements()),
                                           bytes->Length()),
                             NS_ASSIGNMENT_COPY);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return nsCOMPtr<nsISupports>{};
  }

  return nsCOMPtr<nsISupports>(std::move(byteStream));
}

nsresult nsClipboard::EmptyNativeClipboardData(ClipboardType aWhichClipboard) {
  MOZ_DIAGNOSTIC_ASSERT(
      nsIClipboard::IsClipboardTypeSupported(aWhichClipboard));

  if (!jni::IsAvailable()) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  java::Clipboard::Clear(java::GeckoAppShell::GetApplicationContext());

  return NS_OK;
}

mozilla::Result<int32_t, nsresult>
nsClipboard::GetNativeClipboardSequenceNumber(ClipboardType aWhichClipboard) {
  MOZ_DIAGNOSTIC_ASSERT(
      nsIClipboard::IsClipboardTypeSupported(aWhichClipboard));

  if (!jni::IsAvailable()) {
    return Err(NS_ERROR_NOT_AVAILABLE);
  }

  return java::Clipboard::GetSequenceNumber(
      java::GeckoAppShell::GetApplicationContext());
}

mozilla::Result<bool, nsresult>
nsClipboard::HasNativeClipboardDataMatchingFlavors(
    const nsTArray<nsCString>& aFlavorList, ClipboardType aWhichClipboard) {
  MOZ_DIAGNOSTIC_ASSERT(
      nsIClipboard::IsClipboardTypeSupported(aWhichClipboard));

  if (!jni::IsAvailable()) {
    return Err(NS_ERROR_NOT_AVAILABLE);
  }

  for (auto& flavor : aFlavorList) {
    if (java::Clipboard::HasData(java::GeckoAppShell::GetApplicationContext(),
                                 NS_ConvertASCIItoUTF16(flavor))) {
      return true;
    }
  }

  return false;
}
