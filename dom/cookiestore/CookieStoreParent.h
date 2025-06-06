/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_CookieStoreParent_h
#define mozilla_dom_CookieStoreParent_h

#include "mozilla/dom/PCookieStoreParent.h"
#include "mozilla/dom/ContentParent.h"
#include "mozilla/MozPromise.h"

namespace mozilla::dom {

class CookieStoreNotificationWatcher;

class CookieStoreParent final : public PCookieStoreParent {
  friend class PCookieStoreParent;

 public:
  using GetRequestPromise =
      MozPromise<CopyableTArray<CookieStruct>, nsresult, true>;
  using SetDeleteRequestPromise = MozPromise<bool, nsresult, true>;
  using GetSubscriptionsRequestPromise =
      MozPromise<CopyableTArray<CookieSubscription>, nsresult, true>;
  using SubscribeOrUnsubscribeRequestPromise = MozPromise<bool, nsresult, true>;

  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(CookieStoreParent)

  CookieStoreParent();

 private:
  ~CookieStoreParent();

  mozilla::ipc::IPCResult RecvGetRequest(
      nsIURI* aCookieURI, const OriginAttributes& aOriginAttributes,
      const Maybe<OriginAttributes>& aPartitionedOriginAttributes,
      const bool& aThirdPartyContext, const bool& aPartitionForeign,
      const bool& aUsingStorageAccess, const bool& aIsOn3PCBExceptionList,
      const bool& aMatchName, const nsString& aName, const nsCString& aPath,
      const bool& aOnlyFirstMatch, GetRequestResolver&& aResolver);

  mozilla::ipc::IPCResult RecvSetRequest(
      nsIURI* aCookieURI, const OriginAttributes& aOriginAttributes,
      const bool& aThirdPartyContext, const bool& aPartitionForeign,
      const bool& aUsingStorageAccess, const bool& aIsOn3PCBExceptionList,
      const nsString& aName, const nsString& aValue, const bool& aSession,
      const int64_t& aExpires, const nsString& aDomain, const nsString& aPath,
      const int32_t& aSameSite, const bool& aPartitioned,
      const nsID& aOperationID, SetRequestResolver&& aResolver);

  mozilla::ipc::IPCResult RecvDeleteRequest(
      nsIURI* aCookieURI, const OriginAttributes& aOriginAttributes,
      const bool& aThirdPartyContext, const bool& aPartitionForeign,
      const bool& aUsingStorageAccess, const bool& aIsOn3PCBExceptionList,
      const nsString& aName, const nsString& aDomain, const nsString& aPath,
      const bool& aPartitioned, const nsID& aOperationID,
      DeleteRequestResolver&& aResolver);

  mozilla::ipc::IPCResult RecvGetSubscriptionsRequest(
      const PrincipalInfo& aPrincipalInfo, const nsCString& aScopeURL,
      GetSubscriptionsRequestResolver&& aResolver);

  mozilla::ipc::IPCResult RecvSubscribeOrUnsubscribeRequest(
      const PrincipalInfo& aPrincipalInfo, const nsCString& aScopeURL,
      const CopyableTArray<CookieSubscription>& aSubscriptions,
      bool aSubscription, SubscribeOrUnsubscribeRequestResolver&& aResolver);

  mozilla::ipc::IPCResult RecvClose();

  void GetRequestOnMainThread(
      nsIURI* aCookieURI, const OriginAttributes& aOriginAttributes,
      const Maybe<OriginAttributes>& aPartitionedOriginAttributes,
      bool aThirdPartyContext, bool aPartitionForeign, bool aUsingStorageAccess,
      bool aIsOn3PCBExceptionList, bool aMatchName, const nsAString& aName,
      const nsACString& aPath, bool aOnlyFirstMatch,
      nsTArray<CookieStruct>& aResults);

  // Returns true if a cookie notification has been generated while completing
  // the operation.
  bool SetRequestOnMainThread(ThreadsafeContentParentHandle* aParent,
                              nsIURI* aCookieURI, const nsAString& aDomain,
                              const OriginAttributes& aOriginAttributes,
                              bool aThirdPartyContext, bool aPartitionForeign,
                              bool aUsingStorageAccess,
                              bool aIsOn3PCBExceptionList,
                              const nsAString& aName, const nsAString& aValue,
                              bool aSession, int64_t aExpires,
                              const nsAString& aPath, int32_t aSameSite,
                              bool aPartitioned, const nsID& aOperationID);

  // Returns true if a cookie notification has been generated while completing
  // the operation.
  bool DeleteRequestOnMainThread(
      ThreadsafeContentParentHandle* aParent, nsIURI* aCookieURI,
      const nsAString& aDomain, const OriginAttributes& aOriginAttributes,
      bool aThirdPartyContext, bool aPartitionForeign, bool aUsingStorageAccess,
      bool aIsOn3PCBExceptionList, const nsAString& aName,
      const nsAString& aPath, bool aPartitioned, const nsID& aOperationID);

  CookieStoreNotificationWatcher* GetOrCreateNotificationWatcherOnMainThread(
      const OriginAttributes& aOriginAttributes);

  RefPtr<CookieStoreNotificationWatcher> mNotificationWatcherOnMainThread;
};

}  // namespace mozilla::dom

#endif  // mozilla_dom_CookieStoreParent_h
