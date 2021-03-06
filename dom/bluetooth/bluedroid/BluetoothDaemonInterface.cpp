/* -*- Mode: c++; c-basic-offset: 2; indent-tabs-mode: nil; tab-width: 40 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "BluetoothDaemonInterface.h"
#include "BluetoothDaemonHelpers.h"
#include "BluetoothDaemonSetupInterface.h"
#include "BluetoothDaemonSocketInterface.h"
#include "BluetoothInterfaceHelpers.h"
#include "mozilla/unused.h"

using namespace mozilla::ipc;

BEGIN_BLUETOOTH_NAMESPACE

template<class T>
struct interface_traits
{ };

//
// Protocol initialization and setup
//

class BluetoothDaemonSetupModule
{
public:
  virtual nsresult Send(BluetoothDaemonPDU* aPDU, void* aUserData) = 0;

  // Commands
  //

  nsresult RegisterModuleCmd(uint8_t aId, uint8_t aMode,
                             BluetoothSetupResultHandler* aRes)
  {
    MOZ_ASSERT(NS_IsMainThread());

    nsAutoPtr<BluetoothDaemonPDU> pdu(new BluetoothDaemonPDU(0x00, 0x01, 0));

    nsresult rv = PackPDU(aId, aMode, *pdu);
    if (NS_FAILED(rv)) {
      return rv;
    }
    rv = Send(pdu, aRes);
    if (NS_FAILED(rv)) {
      return rv;
    }
    unused << pdu.forget();
    return rv;
  }

  nsresult UnregisterModuleCmd(uint8_t aId,
                               BluetoothSetupResultHandler* aRes)
  {
    MOZ_ASSERT(NS_IsMainThread());

    nsAutoPtr<BluetoothDaemonPDU> pdu(new BluetoothDaemonPDU(0x00, 0x02, 0));

    nsresult rv = PackPDU(aId, *pdu);
    if (NS_FAILED(rv)) {
      return rv;
    }
    rv = Send(pdu, aRes);
    if (NS_FAILED(rv)) {
      return rv;
    }
    unused << pdu.forget();
    return rv;
  }

  nsresult ConfigurationCmd(const BluetoothConfigurationParameter* aParam,
                            uint8_t aLen, BluetoothSetupResultHandler* aRes)
  {
    MOZ_ASSERT(NS_IsMainThread());

    nsAutoPtr<BluetoothDaemonPDU> pdu(new BluetoothDaemonPDU(0x00, 0x03, 0));

    nsresult rv = PackPDU(
      aLen, PackArray<BluetoothConfigurationParameter>(aParam, aLen), *pdu);
    if (NS_FAILED(rv)) {
      return rv;
    }
    rv = Send(pdu, aRes);
    if (NS_FAILED(rv)) {
      return rv;
    }
    unused << pdu.forget();
    return rv;
  }

protected:

  // Called to handle PDUs with Service field equal to 0x00, which
  // contains internal operations for setup and configuration.
  void HandleSvc(const BluetoothDaemonPDUHeader& aHeader,
                 BluetoothDaemonPDU& aPDU, void* aUserData)
  {
    static void (BluetoothDaemonSetupModule::* const HandleRsp[])(
      const BluetoothDaemonPDUHeader&,
      BluetoothDaemonPDU&,
      BluetoothSetupResultHandler*) = {
      INIT_ARRAY_AT(0x00, &BluetoothDaemonSetupModule::ErrorRsp),
      INIT_ARRAY_AT(0x01, &BluetoothDaemonSetupModule::RegisterModuleRsp),
      INIT_ARRAY_AT(0x02, &BluetoothDaemonSetupModule::UnregisterModuleRsp),
      INIT_ARRAY_AT(0x03, &BluetoothDaemonSetupModule::ConfigurationRsp)
    };

    if (NS_WARN_IF(aHeader.mOpcode >= MOZ_ARRAY_LENGTH(HandleRsp)) ||
        NS_WARN_IF(!HandleRsp[aHeader.mOpcode])) {
      return;
    }

    nsRefPtr<BluetoothSetupResultHandler> res =
      already_AddRefed<BluetoothSetupResultHandler>(
        static_cast<BluetoothSetupResultHandler*>(aUserData));

    if (!res) {
      return; // Return early if no result handler has been set
    }

    (this->*(HandleRsp[aHeader.mOpcode]))(aHeader, aPDU, res);
  }

  nsresult Send(BluetoothDaemonPDU* aPDU, BluetoothSetupResultHandler* aRes)
  {
    aRes->AddRef(); // Keep reference for response
    return Send(aPDU, static_cast<void*>(aRes));
  }

private:

  // Responses
  //

  typedef BluetoothResultRunnable0<BluetoothSetupResultHandler, void>
    ResultRunnable;

  typedef BluetoothResultRunnable1<BluetoothSetupResultHandler, void,
                                   BluetoothStatus, BluetoothStatus>
    ErrorRunnable;

  void
  ErrorRsp(const BluetoothDaemonPDUHeader& aHeader,
           BluetoothDaemonPDU& aPDU,
           BluetoothSetupResultHandler* aRes)
  {
    ErrorRunnable::Dispatch(
      aRes, &BluetoothSetupResultHandler::OnError, UnpackPDUInitOp(aPDU));
  }

  void
  RegisterModuleRsp(const BluetoothDaemonPDUHeader& aHeader,
                    BluetoothDaemonPDU& aPDU,
                    BluetoothSetupResultHandler* aRes)
  {
    ResultRunnable::Dispatch(
      aRes, &BluetoothSetupResultHandler::RegisterModule,
      UnpackPDUInitOp(aPDU));
  }

  void
  UnregisterModuleRsp(const BluetoothDaemonPDUHeader& aHeader,
                      BluetoothDaemonPDU& aPDU,
                      BluetoothSetupResultHandler* aRes)
  {
    ResultRunnable::Dispatch(
      aRes, &BluetoothSetupResultHandler::UnregisterModule,
      UnpackPDUInitOp(aPDU));
  }

  void
  ConfigurationRsp(const BluetoothDaemonPDUHeader& aHeader,
                   BluetoothDaemonPDU& aPDU,
                   BluetoothSetupResultHandler* aRes)
  {
    ResultRunnable::Dispatch(
      aRes, &BluetoothSetupResultHandler::Configuration,
      UnpackPDUInitOp(aPDU));
  }
};

//
// Core module
//

static BluetoothNotificationHandler* sNotificationHandler;

class BluetoothDaemonCoreModule
{
public:
  virtual nsresult Send(BluetoothDaemonPDU* aPDU, void* aUserData) = 0;

  nsresult EnableCmd(BluetoothResultHandler* aRes)
  {
    MOZ_ASSERT(NS_IsMainThread());

    nsAutoPtr<BluetoothDaemonPDU> pdu(new BluetoothDaemonPDU(0x01, 0x01, 0));

    nsresult rv = Send(pdu, aRes);
    if (NS_FAILED(rv)) {
      return rv;
    }
    unused << pdu.forget();
    return rv;
  }

  nsresult DisableCmd(BluetoothResultHandler* aRes)
  {
    MOZ_ASSERT(NS_IsMainThread());

    nsAutoPtr<BluetoothDaemonPDU> pdu(new BluetoothDaemonPDU(0x01, 0x02, 0));

    nsresult rv = Send(pdu, aRes);
    if (NS_FAILED(rv)) {
      return rv;
    }
    unused << pdu.forget();
    return rv;
  }

  nsresult GetAdapterPropertiesCmd(BluetoothResultHandler* aRes)
  {
    MOZ_ASSERT(NS_IsMainThread());

    nsAutoPtr<BluetoothDaemonPDU> pdu(new BluetoothDaemonPDU(0x01, 0x03, 0));

    nsresult rv = Send(pdu, aRes);
    if (NS_FAILED(rv)) {
      return rv;
    }
    unused << pdu.forget();
    return rv;
  }

  nsresult GetAdapterPropertyCmd(const nsAString& aName,
                                 BluetoothResultHandler* aRes)
  {
    MOZ_ASSERT(NS_IsMainThread());

    nsAutoPtr<BluetoothDaemonPDU> pdu(new BluetoothDaemonPDU(0x01, 0x04, 0));

    nsresult rv = PackPDU(
      PackConversion<const nsAString, BluetoothPropertyType>(aName), *pdu);
    if (NS_FAILED(rv)) {
      return rv;
    }
    rv = Send(pdu, aRes);
    if (NS_FAILED(rv)) {
      return rv;
    }
    unused << pdu.forget();
    return rv;
  }

  nsresult SetAdapterPropertyCmd(const BluetoothNamedValue& aProperty,
                                 BluetoothResultHandler* aRes)
  {
    MOZ_ASSERT(NS_IsMainThread());

    nsAutoPtr<BluetoothDaemonPDU> pdu(new BluetoothDaemonPDU(0x01, 0x05, 0));

    nsresult rv = PackPDU(aProperty, *pdu);
    if (NS_FAILED(rv)) {
      return rv;
    }
    rv = Send(pdu, aRes);
    if (NS_FAILED(rv)) {
      return rv;
    }
    unused << pdu.forget();
    return rv;
  }

  nsresult GetRemoteDevicePropertiesCmd(const nsAString& aRemoteAddr,
                                        BluetoothResultHandler* aRes)
  {
    MOZ_ASSERT(NS_IsMainThread());

    nsAutoPtr<BluetoothDaemonPDU> pdu(new BluetoothDaemonPDU(0x01, 0x06, 0));

    nsresult rv = PackPDU(
      PackConversion<nsAString, BluetoothAddress>(aRemoteAddr), *pdu);
    if (NS_FAILED(rv)) {
      return rv;
    }
    rv = Send(pdu, aRes);
    if (NS_FAILED(rv)) {
      return rv;
    }
    unused << pdu.forget();
    return rv;
  }

  nsresult GetRemoteDevicePropertyCmd(const nsAString& aRemoteAddr,
                                      const nsAString& aName,
                                      BluetoothResultHandler* aRes)
  {
    MOZ_ASSERT(NS_IsMainThread());

    nsAutoPtr<BluetoothDaemonPDU> pdu(new BluetoothDaemonPDU(0x01, 0x07, 0));

    nsresult rv = PackPDU(
      PackConversion<nsAString, BluetoothAddress>(aRemoteAddr),
      PackConversion<nsAString, BluetoothPropertyType>(aName), *pdu);
    if (NS_FAILED(rv)) {
      return rv;
    }
    rv = Send(pdu, aRes);
    if (NS_FAILED(rv)) {
      return rv;
    }
    unused << pdu.forget();
    return rv;
  }

  nsresult SetRemoteDevicePropertyCmd(const nsAString& aRemoteAddr,
                                      const BluetoothNamedValue& aProperty,
                                      BluetoothResultHandler* aRes)
  {
    MOZ_ASSERT(NS_IsMainThread());

    nsAutoPtr<BluetoothDaemonPDU> pdu(new BluetoothDaemonPDU(0x01, 0x08, 0));

    nsresult rv = PackPDU(
      PackConversion<nsAString, BluetoothAddress>(aRemoteAddr),
      aProperty, *pdu);
    if (NS_FAILED(rv)) {
      return rv;
    }
    rv = Send(pdu, aRes);
    if (NS_FAILED(rv)) {
      return rv;
    }
    unused << pdu.forget();
    return rv;
  }

  nsresult GetRemoteServiceRecordCmd(const nsAString& aRemoteAddr,
                                     const uint8_t aUuid[16],
                                     BluetoothResultHandler* aRes)
  {
    MOZ_ASSERT(NS_IsMainThread());

    nsAutoPtr<BluetoothDaemonPDU> pdu(new BluetoothDaemonPDU(0x01, 0x09, 0));

    nsresult rv = PackPDU(
      PackConversion<nsAString, BluetoothAddress>(aRemoteAddr),
      PackArray<uint8_t>(aUuid, 16), *pdu);
    if (NS_FAILED(rv)) {
      return rv;
    }
    rv = Send(pdu, aRes);
    if (NS_FAILED(rv)) {
      return rv;
    }
    unused << pdu.forget();
    return rv;
  }

  nsresult GetRemoteServicesCmd(const nsAString& aRemoteAddr,
                                BluetoothResultHandler* aRes)
  {
    MOZ_ASSERT(NS_IsMainThread());

    nsAutoPtr<BluetoothDaemonPDU> pdu(new BluetoothDaemonPDU(0x01, 0x0a, 0));

    nsresult rv = PackPDU(
      PackConversion<nsAString, BluetoothAddress>(aRemoteAddr), *pdu);
    if (NS_FAILED(rv)) {
      return rv;
    }
    rv = Send(pdu, aRes);
    if (NS_FAILED(rv)) {
      return rv;
    }
    unused << pdu.forget();
    return rv;
  }

  nsresult StartDiscoveryCmd(BluetoothResultHandler* aRes)
  {
    MOZ_ASSERT(NS_IsMainThread());

    nsAutoPtr<BluetoothDaemonPDU> pdu(new BluetoothDaemonPDU(0x01, 0x0b, 0));

    nsresult rv = Send(pdu, aRes);
    if (NS_FAILED(rv)) {
      return rv;
    }
    unused << pdu.forget();
    return rv;
  }

  nsresult CancelDiscoveryCmd(BluetoothResultHandler* aRes)
  {
    MOZ_ASSERT(NS_IsMainThread());

    nsAutoPtr<BluetoothDaemonPDU> pdu(new BluetoothDaemonPDU(0x01, 0x0c, 0));

    nsresult rv = Send(pdu, aRes);
    if (NS_FAILED(rv)) {
      return rv;
    }
    unused << pdu.forget();
    return rv;
  }

  nsresult CreateBondCmd(const nsAString& aBdAddr,
                         BluetoothResultHandler* aRes)
  {
    MOZ_ASSERT(NS_IsMainThread());

    nsAutoPtr<BluetoothDaemonPDU> pdu(new BluetoothDaemonPDU(0x01, 0x0d, 0));

    nsresult rv = PackPDU(
      PackConversion<nsAString, BluetoothAddress>(aBdAddr), *pdu);
    if (NS_FAILED(rv)) {
      return rv;
    }
    rv = Send(pdu, aRes);
    if (NS_FAILED(rv)) {
      return rv;
    }
    unused << pdu.forget();
    return rv;
  }

  nsresult RemoveBondCmd(const nsAString& aBdAddr,
                         BluetoothResultHandler* aRes)
  {
    MOZ_ASSERT(NS_IsMainThread());

    nsAutoPtr<BluetoothDaemonPDU> pdu(new BluetoothDaemonPDU(0x01, 0x0e, 0));

    nsresult rv = PackPDU(
      PackConversion<nsAString, BluetoothAddress>(aBdAddr), *pdu);
    if (NS_FAILED(rv)) {
      return rv;
    }
    rv = Send(pdu, aRes);
    if (NS_FAILED(rv)) {
      return rv;
    }
    unused << pdu.forget();
    return rv;
  }

  nsresult CancelBondCmd(const nsAString& aBdAddr,
                         BluetoothResultHandler* aRes)
  {
    MOZ_ASSERT(NS_IsMainThread());

    nsAutoPtr<BluetoothDaemonPDU> pdu(new BluetoothDaemonPDU(0x01, 0x0f, 0));

    nsresult rv = PackPDU(
      PackConversion<nsAString, BluetoothAddress>(aBdAddr), *pdu);
    if (NS_FAILED(rv)) {
      return rv;
    }
    rv = Send(pdu, aRes);
    if (NS_FAILED(rv)) {
      return rv;
    }
    unused << pdu.forget();
    return rv;
  }

  nsresult PinReplyCmd(const nsAString& aBdAddr, bool aAccept,
                       const nsAString& aPinCode,
                       BluetoothResultHandler* aRes)
  {
    MOZ_ASSERT(NS_IsMainThread());

    nsAutoPtr<BluetoothDaemonPDU> pdu(new BluetoothDaemonPDU(0x01, 0x10, 0));

    nsresult rv = PackPDU(
      PackConversion<nsAString, BluetoothAddress>(aBdAddr),
      aAccept,
      PackConversion<nsAString, BluetoothPinCode>(aPinCode), *pdu);
    if (NS_FAILED(rv)) {
      return rv;
    }
    rv = Send(pdu, aRes);
    if (NS_FAILED(rv)) {
      return rv;
    }
    unused << pdu.forget();
    return rv;
  }

  nsresult SspReplyCmd(const nsAString& aBdAddr, const nsAString& aVariant,
                       bool aAccept, uint32_t aPasskey,
                       BluetoothResultHandler* aRes)
  {
    MOZ_ASSERT(NS_IsMainThread());

    nsAutoPtr<BluetoothDaemonPDU> pdu(new BluetoothDaemonPDU(0x01, 0x11, 0));

    nsresult rv = PackPDU(
      PackConversion<nsAString, BluetoothAddress>(aBdAddr),
      PackConversion<nsAString, BluetoothSspPairingVariant>(aVariant),
      aAccept, aPasskey, *pdu);
    if (NS_FAILED(rv)) {
      return rv;
    }
    rv = Send(pdu, aRes);
    if (NS_FAILED(rv)) {
      return rv;
    }
    unused << pdu.forget();
    return rv;
  }

  nsresult DutModeConfigureCmd(bool aEnable, BluetoothResultHandler* aRes)
  {
    MOZ_ASSERT(NS_IsMainThread());

    nsAutoPtr<BluetoothDaemonPDU> pdu(new BluetoothDaemonPDU(0x01, 0x12, 0));

    nsresult rv = PackPDU(aEnable, *pdu);
    if (NS_FAILED(rv)) {
      return rv;
    }
    rv = Send(pdu, aRes);
    if (NS_FAILED(rv)) {
      return rv;
    }
    unused << pdu.forget();
    return rv;
  }

  nsresult DutModeSendCmd(uint16_t aOpcode, uint8_t* aBuf, uint8_t aLen,
                          BluetoothResultHandler* aRes)
  {
    MOZ_ASSERT(NS_IsMainThread());

    nsAutoPtr<BluetoothDaemonPDU> pdu(new BluetoothDaemonPDU(0x01, 0x13, 0));

    nsresult rv = PackPDU(aOpcode, aLen, PackArray<uint8_t>(aBuf, aLen),
                          *pdu);
    if (NS_FAILED(rv)) {
      return rv;
    }
    rv = Send(pdu, aRes);
    if (NS_FAILED(rv)) {
      return rv;
    }
    unused << pdu.forget();
    return rv;
  }

  nsresult LeTestModeCmd(uint16_t aOpcode, uint8_t* aBuf, uint8_t aLen,
                         BluetoothResultHandler* aRes)
  {
    MOZ_ASSERT(NS_IsMainThread());

    nsAutoPtr<BluetoothDaemonPDU> pdu(new BluetoothDaemonPDU(0x01, 0x14, 0));

    nsresult rv = PackPDU(aOpcode, aLen, PackArray<uint8_t>(aBuf, aLen),
                          *pdu);
    if (NS_FAILED(rv)) {
      return rv;
    }
    rv = Send(pdu, aRes);
    if (NS_FAILED(rv)) {
      return rv;
    }
    unused << pdu.forget();
    return rv;
  }

protected:

  void HandleSvc(const BluetoothDaemonPDUHeader& aHeader,
                 BluetoothDaemonPDU& aPDU, void* aUserData)
  {
    static void (BluetoothDaemonCoreModule::* const HandleOp[])(
      const BluetoothDaemonPDUHeader&, BluetoothDaemonPDU&, void*) = {
      INIT_ARRAY_AT(0, &BluetoothDaemonCoreModule::HandleRsp),
      INIT_ARRAY_AT(1, &BluetoothDaemonCoreModule::HandleNtf),
    };

    MOZ_ASSERT(!NS_IsMainThread());

    (this->*(HandleOp[!!(aHeader.mOpcode & 0x80)]))(aHeader, aPDU, aUserData);
  }

  nsresult Send(BluetoothDaemonPDU* aPDU, BluetoothResultHandler* aRes)
  {
    aRes->AddRef(); // Keep reference for response
    return Send(aPDU, static_cast<void*>(aRes));
  }

private:

  // Responses
  //

  typedef BluetoothResultRunnable0<BluetoothResultHandler, void>
    ResultRunnable;

  typedef BluetoothResultRunnable1<BluetoothResultHandler, void,
                                   BluetoothStatus, BluetoothStatus>
    ErrorRunnable;

  void ErrorRsp(const BluetoothDaemonPDUHeader& aHeader,
                BluetoothDaemonPDU& aPDU,
                BluetoothResultHandler* aRes)
  {
    ErrorRunnable::Dispatch(
      aRes, &BluetoothResultHandler::OnError, UnpackPDUInitOp(aPDU));
  }

  void EnableRsp(const BluetoothDaemonPDUHeader& aHeader,
                 BluetoothDaemonPDU& aPDU,
                 BluetoothResultHandler* aRes)
  {
    ResultRunnable::Dispatch(
      aRes, &BluetoothResultHandler::Enable, UnpackPDUInitOp(aPDU));
  }

  void DisableRsp(const BluetoothDaemonPDUHeader& aHeader,
                  BluetoothDaemonPDU& aPDU,
                  BluetoothResultHandler* aRes)
  {
    ResultRunnable::Dispatch(
      aRes, &BluetoothResultHandler::Disable, UnpackPDUInitOp(aPDU));
  }

  void GetAdapterPropertiesRsp(const BluetoothDaemonPDUHeader& aHeader,
                               BluetoothDaemonPDU& aPDU,
                               BluetoothResultHandler* aRes)
  {
    ResultRunnable::Dispatch(
      aRes, &BluetoothResultHandler::GetAdapterProperties,
      UnpackPDUInitOp(aPDU));
  }

  void GetAdapterPropertyRsp(const BluetoothDaemonPDUHeader& aHeader,
                             BluetoothDaemonPDU& aPDU,
                             BluetoothResultHandler* aRes)
  {
    ResultRunnable::Dispatch(
      aRes, &BluetoothResultHandler::GetAdapterProperty,
      UnpackPDUInitOp(aPDU));
  }

  void SetAdapterPropertyRsp(const BluetoothDaemonPDUHeader& aHeader,
                             BluetoothDaemonPDU& aPDU,
                             BluetoothResultHandler* aRes)
  {
    ResultRunnable::Dispatch(
      aRes, &BluetoothResultHandler::SetAdapterProperty,
      UnpackPDUInitOp(aPDU));
  }

  void GetRemoteDevicePropertiesRsp(const BluetoothDaemonPDUHeader& aHeader,
                                    BluetoothDaemonPDU& aPDU,
                                    BluetoothResultHandler* aRes)
  {
    ResultRunnable::Dispatch(
      aRes, &BluetoothResultHandler::GetRemoteDeviceProperties,
      UnpackPDUInitOp(aPDU));
  }

  void
  GetRemoteDevicePropertyRsp(const BluetoothDaemonPDUHeader& aHeader,
                             BluetoothDaemonPDU& aPDU,
                             BluetoothResultHandler* aRes)
  {
    ResultRunnable::Dispatch(
      aRes, &BluetoothResultHandler::GetRemoteDeviceProperty,
      UnpackPDUInitOp(aPDU));
  }

  void SetRemoteDevicePropertyRsp(const BluetoothDaemonPDUHeader& aHeader,
                                  BluetoothDaemonPDU& aPDU,
                                  BluetoothResultHandler* aRes)
  {
    ResultRunnable::Dispatch(
      aRes, &BluetoothResultHandler::SetRemoteDeviceProperty,
      UnpackPDUInitOp(aPDU));
  }

  void GetRemoteServiceRecordRsp(const BluetoothDaemonPDUHeader& aHeader,
                                 BluetoothDaemonPDU& aPDU,
                                 BluetoothResultHandler* aRes)
  {
    ResultRunnable::Dispatch(
      aRes, &BluetoothResultHandler::GetRemoteServiceRecord,
      UnpackPDUInitOp(aPDU));
  }

  void GetRemoteServicesRsp(const BluetoothDaemonPDUHeader& aHeader,
                            BluetoothDaemonPDU& aPDU,
                            BluetoothResultHandler* aRes)
  {
    ResultRunnable::Dispatch(
      aRes, &BluetoothResultHandler::GetRemoteServices,
      UnpackPDUInitOp(aPDU));
  }

  void StartDiscoveryRsp(const BluetoothDaemonPDUHeader& aHeader,
                         BluetoothDaemonPDU& aPDU,
                         BluetoothResultHandler* aRes)
  {
    ResultRunnable::Dispatch(
      aRes, &BluetoothResultHandler::StartDiscovery,
      UnpackPDUInitOp(aPDU));
  }

  void CancelDiscoveryRsp(const BluetoothDaemonPDUHeader& aHeader,
                          BluetoothDaemonPDU& aPDU,
                          BluetoothResultHandler* aRes)
  {
    ResultRunnable::Dispatch(
      aRes, &BluetoothResultHandler::CancelDiscovery,
      UnpackPDUInitOp(aPDU));
  }

  void CreateBondRsp(const BluetoothDaemonPDUHeader& aHeader,
                     BluetoothDaemonPDU& aPDU,
                     BluetoothResultHandler* aRes)
  {
    ResultRunnable::Dispatch(
      aRes, &BluetoothResultHandler::CreateBond,
      UnpackPDUInitOp(aPDU));
  }

  void RemoveBondRsp(const BluetoothDaemonPDUHeader& aHeader,
                     BluetoothDaemonPDU& aPDU,
                     BluetoothResultHandler* aRes)
  {
    ResultRunnable::Dispatch(
      aRes, &BluetoothResultHandler::RemoveBond,
      UnpackPDUInitOp(aPDU));
  }

  void CancelBondRsp(const BluetoothDaemonPDUHeader& aHeader,
                     BluetoothDaemonPDU& aPDU,
                     BluetoothResultHandler* aRes)
  {
    ResultRunnable::Dispatch(
      aRes, &BluetoothResultHandler::CancelBond,
      UnpackPDUInitOp(aPDU));
  }

  void PinReplyRsp(const BluetoothDaemonPDUHeader& aHeader,
                   BluetoothDaemonPDU& aPDU,
                   BluetoothResultHandler* aRes)
  {
    ResultRunnable::Dispatch(
      aRes, &BluetoothResultHandler::PinReply,
      UnpackPDUInitOp(aPDU));
  }

  void SspReplyRsp(const BluetoothDaemonPDUHeader& aHeader,
                   BluetoothDaemonPDU& aPDU,
                   BluetoothResultHandler* aRes)
  {
    ResultRunnable::Dispatch(
      aRes, &BluetoothResultHandler::SspReply,
      UnpackPDUInitOp(aPDU));
  }

  void DutModeConfigureRsp(const BluetoothDaemonPDUHeader& aHeader,
                           BluetoothDaemonPDU& aPDU,
                           BluetoothResultHandler* aRes)
  {
    ResultRunnable::Dispatch(
      aRes, &BluetoothResultHandler::DutModeConfigure,
      UnpackPDUInitOp(aPDU));
  }

  void DutModeSendRsp(const BluetoothDaemonPDUHeader& aHeader,
                      BluetoothDaemonPDU& aPDU,
                      BluetoothResultHandler* aRes)
  {
    ResultRunnable::Dispatch(
      aRes, &BluetoothResultHandler::DutModeSend,
      UnpackPDUInitOp(aPDU));
  }

  void LeTestModeRsp(const BluetoothDaemonPDUHeader& aHeader,
                     BluetoothDaemonPDU& aPDU,
                     BluetoothResultHandler* aRes)
  {
    ResultRunnable::Dispatch(
      aRes, &BluetoothResultHandler::LeTestMode,
      UnpackPDUInitOp(aPDU));
  }

  void HandleRsp(const BluetoothDaemonPDUHeader& aHeader,
                 BluetoothDaemonPDU& aPDU, void* aUserData)
  {
    static void (BluetoothDaemonCoreModule::* const HandleRsp[])(
      const BluetoothDaemonPDUHeader&,
      BluetoothDaemonPDU&,
      BluetoothResultHandler*) = {
      INIT_ARRAY_AT(0x00, &BluetoothDaemonCoreModule::ErrorRsp),
      INIT_ARRAY_AT(0x01, &BluetoothDaemonCoreModule::EnableRsp),
      INIT_ARRAY_AT(0x02, &BluetoothDaemonCoreModule::DisableRsp),
      INIT_ARRAY_AT(0x03, &BluetoothDaemonCoreModule::GetAdapterPropertiesRsp),
      INIT_ARRAY_AT(0x04, &BluetoothDaemonCoreModule::GetAdapterPropertyRsp),
      INIT_ARRAY_AT(0x05, &BluetoothDaemonCoreModule::SetAdapterPropertyRsp),
      INIT_ARRAY_AT(0x06,
        &BluetoothDaemonCoreModule::GetRemoteDevicePropertiesRsp),
      INIT_ARRAY_AT(0x07,
        &BluetoothDaemonCoreModule::GetRemoteDevicePropertyRsp),
      INIT_ARRAY_AT(0x08,
        &BluetoothDaemonCoreModule::SetRemoteDevicePropertyRsp),
      INIT_ARRAY_AT(0x09,
        &BluetoothDaemonCoreModule::GetRemoteServiceRecordRsp),
      INIT_ARRAY_AT(0x0a, &BluetoothDaemonCoreModule::GetRemoteServicesRsp),
      INIT_ARRAY_AT(0x0b, &BluetoothDaemonCoreModule::StartDiscoveryRsp),
      INIT_ARRAY_AT(0x0c, &BluetoothDaemonCoreModule::CancelDiscoveryRsp),
      INIT_ARRAY_AT(0x0d, &BluetoothDaemonCoreModule::CreateBondRsp),
      INIT_ARRAY_AT(0x0e, &BluetoothDaemonCoreModule::RemoveBondRsp),
      INIT_ARRAY_AT(0x0f, &BluetoothDaemonCoreModule::CancelBondRsp),
      INIT_ARRAY_AT(0x10, &BluetoothDaemonCoreModule::PinReplyRsp),
      INIT_ARRAY_AT(0x11, &BluetoothDaemonCoreModule::SspReplyRsp),
      INIT_ARRAY_AT(0x12, &BluetoothDaemonCoreModule::DutModeConfigureRsp),
      INIT_ARRAY_AT(0x13, &BluetoothDaemonCoreModule::DutModeSendRsp),
      INIT_ARRAY_AT(0x14, &BluetoothDaemonCoreModule::LeTestModeRsp),
    };

    MOZ_ASSERT(!NS_IsMainThread());

    if (NS_WARN_IF(!(aHeader.mOpcode < MOZ_ARRAY_LENGTH(HandleRsp))) ||
        NS_WARN_IF(!HandleRsp[aHeader.mOpcode])) {
      return;
    }

    nsRefPtr<BluetoothResultHandler> res =
      already_AddRefed<BluetoothResultHandler>(
        static_cast<BluetoothResultHandler*>(aUserData));

    if (!res) {
      return; // Return early if no result handler has been set for response
    }

    (this->*(HandleRsp[aHeader.mOpcode]))(aHeader, aPDU, res);
  }

  // Notifications
  //

  class NotificationHandlerWrapper
  {
  public:
    typedef BluetoothNotificationHandler ObjectType;

    static ObjectType* GetInstance()
    {
      MOZ_ASSERT(NS_IsMainThread());

      return sNotificationHandler;
    }
  };

  typedef BluetoothNotificationRunnable1<NotificationHandlerWrapper, void,
                                         bool>
    AdapterStateChangedNotification;

  typedef BluetoothNotificationRunnable3<NotificationHandlerWrapper, void,
                                         BluetoothStatus, int,
                                         nsAutoArrayPtr<BluetoothProperty>,
                                         BluetoothStatus, int,
                                         const BluetoothProperty*>
    AdapterPropertiesNotification;

  typedef BluetoothNotificationRunnable4<NotificationHandlerWrapper, void,
                                         BluetoothStatus, nsString, int,
                                         nsAutoArrayPtr<BluetoothProperty>,
                                         BluetoothStatus, const nsAString&,
                                         int, const BluetoothProperty*>
    RemoteDevicePropertiesNotification;

  typedef BluetoothNotificationRunnable2<NotificationHandlerWrapper, void,
                                         int,
                                         nsAutoArrayPtr<BluetoothProperty>,
                                         int, const BluetoothProperty*>
    DeviceFoundNotification;

  typedef BluetoothNotificationRunnable1<NotificationHandlerWrapper, void,
                                         bool>
    DiscoveryStateChangedNotification;

  typedef BluetoothNotificationRunnable3<NotificationHandlerWrapper, void,
                                         nsString, nsString, uint32_t,
                                         const nsAString&, const nsAString&>
    PinRequestNotification;

  typedef BluetoothNotificationRunnable5<NotificationHandlerWrapper, void,
                                         nsString, nsString, uint32_t,
                                         nsString, uint32_t,
                                         const nsAString&, const nsAString&,
                                         uint32_t, const nsAString&>
    SspRequestNotification;

  typedef BluetoothNotificationRunnable3<NotificationHandlerWrapper, void,
                                         BluetoothStatus, nsString,
                                         BluetoothBondState,
                                         BluetoothStatus, const nsAString&>
    BondStateChangedNotification;

  typedef BluetoothNotificationRunnable3<NotificationHandlerWrapper, void,
                                         BluetoothStatus, nsString, bool,
                                         BluetoothStatus, const nsAString&>
    AclStateChangedNotification;

  typedef BluetoothNotificationRunnable3<NotificationHandlerWrapper, void,
                                         uint16_t, nsAutoArrayPtr<uint8_t>,
                                         uint8_t, uint16_t, const uint8_t*>
    DutModeRecvNotification;

  typedef BluetoothNotificationRunnable2<NotificationHandlerWrapper, void,
                                         BluetoothStatus, uint16_t>
    LeTestModeNotification;

  void AdapterStateChangedNtf(const BluetoothDaemonPDUHeader& aHeader,
                              BluetoothDaemonPDU& aPDU)
  {
    AdapterStateChangedNotification::Dispatch(
      &BluetoothNotificationHandler::AdapterStateChangedNotification,
      UnpackPDUInitOp(aPDU));
  }

  // Init operator class for AdapterPropertiesNotification
  class AdapterPropertiesInitOp MOZ_FINAL : private PDUInitOp
  {
  public:
    AdapterPropertiesInitOp(BluetoothDaemonPDU& aPDU)
    : PDUInitOp(aPDU)
    { }

    nsresult
    operator () (BluetoothStatus& aArg1, int& aArg2,
                 nsAutoArrayPtr<BluetoothProperty>& aArg3) const
    {
      BluetoothDaemonPDU& pdu = GetPDU();

      /* Read status */
      nsresult rv = UnpackPDU(pdu, aArg1);
      if (NS_FAILED(rv)) {
        return rv;
      }

      /* Read number of properties */
      uint8_t numProperties;
      rv = UnpackPDU(pdu, numProperties);
      if (NS_FAILED(rv)) {
        return rv;
      }
      aArg2 = numProperties;

      /* Read properties array */
      UnpackArray<BluetoothProperty> properties(aArg3, aArg2);
      rv = UnpackPDU(pdu, properties);
      if (NS_FAILED(rv)) {
        return rv;
      }
      WarnAboutTrailingData();
      return NS_OK;
    }
  };

  void AdapterPropertiesNtf(const BluetoothDaemonPDUHeader& aHeader,
                            BluetoothDaemonPDU& aPDU)
  {
    AdapterPropertiesNotification::Dispatch(
      &BluetoothNotificationHandler::AdapterPropertiesNotification,
      AdapterPropertiesInitOp(aPDU));
  }

  // Init operator class for RemoteDevicePropertiesNotification
  class RemoteDevicePropertiesInitOp MOZ_FINAL : private PDUInitOp
  {
  public:
    RemoteDevicePropertiesInitOp(BluetoothDaemonPDU& aPDU)
    : PDUInitOp(aPDU)
    { }

    nsresult
    operator () (BluetoothStatus& aArg1, nsString& aArg2, int& aArg3,
                 nsAutoArrayPtr<BluetoothProperty>& aArg4) const
    {
      BluetoothDaemonPDU& pdu = GetPDU();

      /* Read status */
      nsresult rv = UnpackPDU(pdu, aArg1);
      if (NS_FAILED(rv)) {
        return rv;
      }

      /* Read address */
      rv = UnpackPDU(
        pdu, UnpackConversion<BluetoothAddress, nsAString>(aArg2));
      if (NS_FAILED(rv)) {
        return rv;
      }

      /* Read number of properties */
      uint8_t numProperties;
      rv = UnpackPDU(pdu, numProperties);
      if (NS_FAILED(rv)) {
        return rv;
      }
      aArg3 = numProperties;

      /* Read properties array */
      UnpackArray<BluetoothProperty> properties(aArg4, aArg3);
      rv = UnpackPDU(pdu, properties);
      if (NS_FAILED(rv)) {
        return rv;
      }
      WarnAboutTrailingData();
      return NS_OK;
    }
  };

  void RemoteDevicePropertiesNtf(const BluetoothDaemonPDUHeader& aHeader,
                                 BluetoothDaemonPDU& aPDU)
  {
    RemoteDevicePropertiesNotification::Dispatch(
      &BluetoothNotificationHandler::RemoteDevicePropertiesNotification,
      RemoteDevicePropertiesInitOp(aPDU));
  }

  // Init operator class for DeviceFoundNotification
  class DeviceFoundInitOp MOZ_FINAL : private PDUInitOp
  {
  public:
    DeviceFoundInitOp(BluetoothDaemonPDU& aPDU)
    : PDUInitOp(aPDU)
    { }

    nsresult
    operator () (int& aArg1, nsAutoArrayPtr<BluetoothProperty>& aArg2) const
    {
      BluetoothDaemonPDU& pdu = GetPDU();

      /* Read number of properties */
      uint8_t numProperties;
      nsresult rv = UnpackPDU(pdu, numProperties);
      if (NS_FAILED(rv)) {
        return rv;
      }
      aArg1 = numProperties;

      /* Read properties array */
      UnpackArray<BluetoothProperty> properties(aArg2, aArg1);
      rv = UnpackPDU(pdu, properties);
      if (NS_FAILED(rv)) {
        return rv;
      }
      WarnAboutTrailingData();
      return NS_OK;
    }
  };

  void DeviceFoundNtf(const BluetoothDaemonPDUHeader& aHeader,
                      BluetoothDaemonPDU& aPDU)
  {
    DeviceFoundNotification::Dispatch(
      &BluetoothNotificationHandler::DeviceFoundNotification,
      DeviceFoundInitOp(aPDU));
  }

  void DiscoveryStateChangedNtf(const BluetoothDaemonPDUHeader& aHeader,
                                BluetoothDaemonPDU& aPDU)
  {
    DiscoveryStateChangedNotification::Dispatch(
      &BluetoothNotificationHandler::DiscoveryStateChangedNotification,
      UnpackPDUInitOp(aPDU));
  }

  // Init operator class for PinRequestNotification
  class PinRequestInitOp MOZ_FINAL : private PDUInitOp
  {
  public:
    PinRequestInitOp(BluetoothDaemonPDU& aPDU)
    : PDUInitOp(aPDU)
    { }

    nsresult
    operator () (nsString& aArg1, nsString& aArg2, uint32_t& aArg3) const
    {
      BluetoothDaemonPDU& pdu = GetPDU();

      /* Read remote address */
      nsresult rv = UnpackPDU(
        pdu, UnpackConversion<BluetoothAddress, nsAString>(aArg1));
      if (NS_FAILED(rv)) {
        return rv;
      }

      /* Read remote name */
      rv = UnpackPDU(
        pdu, UnpackConversion<BluetoothRemoteName, nsAString>(aArg2));
      if (NS_FAILED(rv)) {
        return rv;
      }

      /* Read CoD */
      rv = UnpackPDU(pdu, aArg3);
      if (NS_FAILED(rv)) {
        return rv;
      }
      WarnAboutTrailingData();
      return NS_OK;
    }
  };

  void PinRequestNtf(const BluetoothDaemonPDUHeader& aHeader,
                     BluetoothDaemonPDU& aPDU)
  {
    PinRequestNotification::Dispatch(
      &BluetoothNotificationHandler::PinRequestNotification,
      PinRequestInitOp(aPDU));
  }

  // Init operator class for SspRequestNotification
  class SspRequestInitOp MOZ_FINAL : private PDUInitOp
  {
  public:
    SspRequestInitOp(BluetoothDaemonPDU& aPDU)
    : PDUInitOp(aPDU)
    { }

    nsresult
    operator () (nsString& aArg1, nsString& aArg2, uint32_t& aArg3,
                 nsString& aArg4, uint32_t& aArg5) const
    {
      BluetoothDaemonPDU& pdu = GetPDU();

      /* Read remote address */
      nsresult rv = UnpackPDU(
        pdu, UnpackConversion<BluetoothAddress, nsAString>(aArg1));
      if (NS_FAILED(rv)) {
        return rv;
      }

      /* Read remote name */
      rv = UnpackPDU(
        pdu, UnpackConversion<BluetoothRemoteName, nsAString>(aArg2));
      if (NS_FAILED(rv)) {
        return rv;
      }

      /* Read CoD */
      rv = UnpackPDU(pdu, aArg3);
      if (NS_FAILED(rv)) {
        return rv;
      }

      /* Read pairing variant */
      rv = UnpackPDU(
        pdu, UnpackConversion<BluetoothSspPairingVariant, nsAString>(aArg4));
      if (NS_FAILED(rv)) {
        return rv;
      }

      /* Read passkey */
      rv = UnpackPDU(pdu, aArg5);
      if (NS_FAILED(rv)) {
        return rv;
      }
      WarnAboutTrailingData();
      return NS_OK;
    }
  };

  void SspRequestNtf(const BluetoothDaemonPDUHeader& aHeader,
                     BluetoothDaemonPDU& aPDU)
  {
    SspRequestNotification::Dispatch(
      &BluetoothNotificationHandler::SspRequestNotification,
      SspRequestInitOp(aPDU));
  }

  // Init operator class for BondStateChangedNotification
  class BondStateChangedInitOp MOZ_FINAL : private PDUInitOp
  {
  public:
    BondStateChangedInitOp(BluetoothDaemonPDU& aPDU)
    : PDUInitOp(aPDU)
    { }

    nsresult
    operator () (BluetoothStatus& aArg1, nsString& aArg2,
                 BluetoothBondState& aArg3) const
    {
      BluetoothDaemonPDU& pdu = GetPDU();

      /* Read status */
      nsresult rv = UnpackPDU(pdu, aArg1);
      if (NS_FAILED(rv)) {
        return rv;
      }

      /* Read remote address */
      rv = UnpackPDU(
        pdu, UnpackConversion<BluetoothAddress, nsAString>(aArg2));
      if (NS_FAILED(rv)) {
        return rv;
      }

      /* Read bond state */
      rv = UnpackPDU(pdu, aArg3);
      if (NS_FAILED(rv)) {
        return rv;
      }
      WarnAboutTrailingData();
      return NS_OK;
    }
  };

  void BondStateChangedNtf(const BluetoothDaemonPDUHeader& aHeader,
                           BluetoothDaemonPDU& aPDU)
  {
    BondStateChangedNotification::Dispatch(
      &BluetoothNotificationHandler::BondStateChangedNotification,
      BondStateChangedInitOp(aPDU));
  }

  // Init operator class for AclStateChangedNotification
  class AclStateChangedInitOp MOZ_FINAL : private PDUInitOp
  {
  public:
    AclStateChangedInitOp(BluetoothDaemonPDU& aPDU)
    : PDUInitOp(aPDU)
    { }

    nsresult
    operator () (BluetoothStatus& aArg1, nsString& aArg2, bool& aArg3) const
    {
      BluetoothDaemonPDU& pdu = GetPDU();

      /* Read status */
      nsresult rv = UnpackPDU(pdu, aArg1);
      if (NS_FAILED(rv)) {
        return rv;
      }

      /* Read remote address */
      rv = UnpackPDU(
        pdu, UnpackConversion<BluetoothAddress, nsAString>(aArg2));
      if (NS_FAILED(rv)) {
        return rv;
      }

      /* Read ACL state */
      rv = UnpackPDU(
        pdu, UnpackConversion<BluetoothAclState, bool>(aArg3));
      if (NS_FAILED(rv)) {
        return rv;
      }
      WarnAboutTrailingData();
      return NS_OK;
    }
  };

  void AclStateChangedNtf(const BluetoothDaemonPDUHeader& aHeader,
                          BluetoothDaemonPDU& aPDU)
  {
    AclStateChangedNotification::Dispatch(
      &BluetoothNotificationHandler::AclStateChangedNotification,
      AclStateChangedInitOp(aPDU));
  }

  // Init operator class for DutModeRecvNotification
  class DutModeRecvInitOp MOZ_FINAL : private PDUInitOp
  {
  public:
    DutModeRecvInitOp(BluetoothDaemonPDU& aPDU)
    : PDUInitOp(aPDU)
    { }

    nsresult
    operator () (uint16_t& aArg1, nsAutoArrayPtr<uint8_t>& aArg2,
                 uint8_t& aArg3) const
    {
      BluetoothDaemonPDU& pdu = GetPDU();

      /* Read opcode */
      nsresult rv = UnpackPDU(pdu, aArg1);
      if (NS_FAILED(rv)) {
        return rv;
      }

      /* Read length */
      rv = UnpackPDU(pdu, aArg3);
      if (NS_FAILED(rv)) {
        return rv;
      }

      /* Read data */
      rv = UnpackPDU(pdu, UnpackArray<uint8_t>(aArg2, aArg3));
      if (NS_FAILED(rv)) {
        return rv;
      }
      WarnAboutTrailingData();
      return NS_OK;
    }
  };

  void DutModeRecvNtf(const BluetoothDaemonPDUHeader& aHeader,
                      BluetoothDaemonPDU& aPDU)
  {
    DutModeRecvNotification::Dispatch(
      &BluetoothNotificationHandler::DutModeRecvNotification,
      DutModeRecvInitOp(aPDU));
  }

  void LeTestModeNtf(const BluetoothDaemonPDUHeader& aHeader,
                     BluetoothDaemonPDU& aPDU)
  {
    LeTestModeNotification::Dispatch(
      &BluetoothNotificationHandler::LeTestModeNotification,
      UnpackPDUInitOp(aPDU));
  }

  void HandleNtf(const BluetoothDaemonPDUHeader& aHeader,
                 BluetoothDaemonPDU& aPDU, void* aUserData)
  {
    static void (BluetoothDaemonCoreModule::* const HandleNtf[])(
      const BluetoothDaemonPDUHeader&, BluetoothDaemonPDU&) = {
      INIT_ARRAY_AT(0, &BluetoothDaemonCoreModule::AdapterStateChangedNtf),
      INIT_ARRAY_AT(1, &BluetoothDaemonCoreModule::AdapterPropertiesNtf),
      INIT_ARRAY_AT(2, &BluetoothDaemonCoreModule::RemoteDevicePropertiesNtf),
      INIT_ARRAY_AT(3, &BluetoothDaemonCoreModule::DeviceFoundNtf),
      INIT_ARRAY_AT(4, &BluetoothDaemonCoreModule::DiscoveryStateChangedNtf),
      INIT_ARRAY_AT(5, &BluetoothDaemonCoreModule::PinRequestNtf),
      INIT_ARRAY_AT(6, &BluetoothDaemonCoreModule::SspRequestNtf),
      INIT_ARRAY_AT(7, &BluetoothDaemonCoreModule::BondStateChangedNtf),
      INIT_ARRAY_AT(8, &BluetoothDaemonCoreModule::AclStateChangedNtf),
      INIT_ARRAY_AT(9, &BluetoothDaemonCoreModule::DutModeRecvNtf),
      INIT_ARRAY_AT(10, &BluetoothDaemonCoreModule::LeTestModeNtf)
    };

    MOZ_ASSERT(!NS_IsMainThread());

    uint8_t index = aHeader.mOpcode - 0x81;

    if (NS_WARN_IF(!(index < MOZ_ARRAY_LENGTH(HandleNtf))) ||
        NS_WARN_IF(!HandleNtf[index])) {
      return;
    }

    (this->*(HandleNtf[index]))(aHeader, aPDU);
  }

};

//
// Protocol handling
//

class BluetoothDaemonProtocol MOZ_FINAL
  : public BluetoothDaemonPDUConsumer
  , public BluetoothDaemonSetupModule
  , public BluetoothDaemonCoreModule
  , public BluetoothDaemonSocketModule
{
public:
  BluetoothDaemonProtocol(BluetoothDaemonConnection* aConnection);

  // Outgoing PDUs
  //

  nsresult Send(BluetoothDaemonPDU* aPDU, void* aUserData) MOZ_OVERRIDE;

  void StoreUserData(const BluetoothDaemonPDU& aPDU) MOZ_OVERRIDE;

  // Incoming PUDs
  //

  void Handle(BluetoothDaemonPDU& aPDU) MOZ_OVERRIDE;

  void* FetchUserData(const BluetoothDaemonPDUHeader& aHeader);

private:
  void HandleSetupSvc(const BluetoothDaemonPDUHeader& aHeader,
                      BluetoothDaemonPDU& aPDU, void* aUserData);
  void HandleCoreSvc(const BluetoothDaemonPDUHeader& aHeader,
                     BluetoothDaemonPDU& aPDU, void* aUserData);
  void HandleSocketSvc(const BluetoothDaemonPDUHeader& aHeader,
                       BluetoothDaemonPDU& aPDU, void* aUserData);

  BluetoothDaemonConnection* mConnection;
  nsTArray<void*> mUserDataQ;
};

BluetoothDaemonProtocol::BluetoothDaemonProtocol(
  BluetoothDaemonConnection* aConnection)
  : mConnection(aConnection)
{
  MOZ_ASSERT(mConnection);
}

nsresult
BluetoothDaemonProtocol::Send(BluetoothDaemonPDU* aPDU, void* aUserData)
{
  MOZ_ASSERT(aPDU);

  aPDU->SetUserData(aUserData);
  aPDU->UpdateHeader();
  return mConnection->Send(aPDU); // Forward PDU to command channel
}

void
BluetoothDaemonProtocol::HandleSetupSvc(
  const BluetoothDaemonPDUHeader& aHeader, BluetoothDaemonPDU& aPDU,
  void* aUserData)
{
  BluetoothDaemonSetupModule::HandleSvc(aHeader, aPDU, aUserData);
}

void
BluetoothDaemonProtocol::HandleCoreSvc(
  const BluetoothDaemonPDUHeader& aHeader, BluetoothDaemonPDU& aPDU,
  void* aUserData)
{
  BluetoothDaemonCoreModule::HandleSvc(aHeader, aPDU, aUserData);
}

void
BluetoothDaemonProtocol::HandleSocketSvc(
  const BluetoothDaemonPDUHeader& aHeader, BluetoothDaemonPDU& aPDU,
  void* aUserData)
{
  BluetoothDaemonSocketModule::HandleSvc(aHeader, aPDU, aUserData);
}

void
BluetoothDaemonProtocol::Handle(BluetoothDaemonPDU& aPDU)
{
  static void (BluetoothDaemonProtocol::* const HandleSvc[])(
    const BluetoothDaemonPDUHeader&, BluetoothDaemonPDU&, void*) = {
    INIT_ARRAY_AT(0x00, &BluetoothDaemonProtocol::HandleSetupSvc),
    INIT_ARRAY_AT(0x01, &BluetoothDaemonProtocol::HandleCoreSvc),
    INIT_ARRAY_AT(0x02, &BluetoothDaemonProtocol::HandleSocketSvc)
  };

  BluetoothDaemonPDUHeader header;

  if (NS_FAILED(UnpackPDU(aPDU, header)) ||
      NS_WARN_IF(!(header.mService < MOZ_ARRAY_LENGTH(HandleSvc))) ||
      NS_WARN_IF(!(HandleSvc[header.mService]))) {
    return;
  }

  (this->*(HandleSvc[header.mService]))(header, aPDU, FetchUserData(header));
}

void
BluetoothDaemonProtocol::StoreUserData(const BluetoothDaemonPDU& aPDU)
{
  MOZ_ASSERT(!NS_IsMainThread());

  mUserDataQ.AppendElement(aPDU.GetUserData());
}

void*
BluetoothDaemonProtocol::FetchUserData(const BluetoothDaemonPDUHeader& aHeader)
{
  MOZ_ASSERT(!NS_IsMainThread());

  if (aHeader.mOpcode & 0x80) {
    return nullptr; // Ignore notifications
  }

  void* userData = mUserDataQ.ElementAt(0);
  mUserDataQ.RemoveElementAt(0);

  return userData;
}

//
// Channels
//

class BluetoothDaemonChannel MOZ_FINAL : public BluetoothDaemonConnection
{
public:
  BluetoothDaemonChannel(BluetoothDaemonInterface::Channel aChannel);

  nsresult ConnectSocket(BluetoothDaemonInterface* aInterface,
                         BluetoothDaemonPDUConsumer* aConsumer);

  // Connection state
  //

  void OnConnectSuccess() MOZ_OVERRIDE;
  void OnConnectError() MOZ_OVERRIDE;
  void OnDisconnect() MOZ_OVERRIDE;

private:
  BluetoothDaemonInterface* mInterface;
  BluetoothDaemonInterface::Channel mChannel;
};

BluetoothDaemonChannel::BluetoothDaemonChannel(
  BluetoothDaemonInterface::Channel aChannel)
: mInterface(nullptr)
, mChannel(aChannel)
{ }

nsresult
BluetoothDaemonChannel::ConnectSocket(BluetoothDaemonInterface* aInterface,
                                      BluetoothDaemonPDUConsumer* aConsumer)
{
  MOZ_ASSERT(aInterface);

  mInterface = aInterface;

  return BluetoothDaemonConnection::ConnectSocket(aConsumer);
}

void
BluetoothDaemonChannel::OnConnectSuccess()
{
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(mInterface);

  mInterface->OnConnectSuccess(mChannel);
}

void
BluetoothDaemonChannel::OnConnectError()
{
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(mInterface);

  mInterface->OnConnectError(mChannel);
  mInterface = nullptr;
}

void
BluetoothDaemonChannel::OnDisconnect()
{
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(mInterface);

  mInterface->OnDisconnect(mChannel);
  mInterface = nullptr;
}

//
// Interface
//

/* returns the container structure of a variable; _t is the container's
 * type, _v the name of the variable, and _m is _v's field within _t
 */
#define container(_t, _v, _m) \
  ( (_t*)( ((const unsigned char*)(_v)) - offsetof(_t, _m) ) )

BluetoothDaemonInterface*
BluetoothDaemonInterface::GetInstance()
{
  static BluetoothDaemonInterface* sBluetoothInterface;

  if (sBluetoothInterface) {
    return sBluetoothInterface;
  }

  // Only create channel objects here. The connection will be
  // established by |BluetoothDaemonInterface::Init|.

  BluetoothDaemonChannel* cmdChannel =
    new BluetoothDaemonChannel(BluetoothDaemonInterface::CMD_CHANNEL);

  BluetoothDaemonChannel* ntfChannel =
    new BluetoothDaemonChannel(BluetoothDaemonInterface::NTF_CHANNEL);

  // Create a new interface object with the channels and a
  // protocol handler.

  sBluetoothInterface =
    new BluetoothDaemonInterface(cmdChannel,
                                 ntfChannel,
                                 new BluetoothDaemonProtocol(cmdChannel));

  return sBluetoothInterface;
}

BluetoothDaemonInterface::BluetoothDaemonInterface(
  BluetoothDaemonChannel* aCmdChannel,
  BluetoothDaemonChannel* aNtfChannel,
  BluetoothDaemonProtocol* aProtocol)
: mCmdChannel(aCmdChannel)
, mNtfChannel(aNtfChannel)
, mProtocol(aProtocol)
{
  MOZ_ASSERT(mCmdChannel);
  MOZ_ASSERT(mNtfChannel);
  MOZ_ASSERT(mProtocol);
}

BluetoothDaemonInterface::~BluetoothDaemonInterface()
{ }

class BluetoothDaemonInterface::InitResultHandler MOZ_FINAL
  : public BluetoothSetupResultHandler
{
public:
  InitResultHandler(BluetoothDaemonInterface* aInterface,
                    BluetoothResultHandler* aRes)
    : mInterface(aInterface)
    , mRes(aRes)
    , mRegisteredSocketModule(false)
  {
    MOZ_ASSERT(mInterface);
  }

  // We need to call methods from the |BluetoothResultHandler|. Since
  // we're already on the main thread and returned from Init, we don't
  // need to dispatch a new runnable.

  void OnError(BluetoothStatus aStatus) MOZ_OVERRIDE
  {
    MOZ_ASSERT(NS_IsMainThread());

    if (mRes) {
      mRes->OnError(aStatus);
    }
  }

  void RegisterModule() MOZ_OVERRIDE
  {
    MOZ_ASSERT(NS_IsMainThread());
    MOZ_ASSERT(mInterface->mProtocol);

    if (!mRegisteredSocketModule) {
      mRegisteredSocketModule = true;
      // Init, step 4: Register Socket module
      mInterface->mProtocol->RegisterModuleCmd(0x02, 0x00, this);
    } else if (mRes) {
      // Init, step 5: Signal success to caller
      mRes->Init();
    }
  }

private:
  BluetoothDaemonInterface* mInterface;
  nsRefPtr<BluetoothResultHandler> mRes;
  bool mRegisteredSocketModule;
};

void
BluetoothDaemonInterface::OnConnectSuccess(enum Channel aChannel)
{
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(!mResultHandlerQ.IsEmpty());

  switch (aChannel) {
    case CMD_CHANNEL:
      // Init, step 2: Connect notification channel...
      if (mNtfChannel->GetConnectionStatus() != SOCKET_CONNECTED) {
        nsresult rv = mNtfChannel->ConnectSocket(this, mProtocol);
        if (NS_FAILED(rv)) {
          OnConnectError(CMD_CHANNEL);
        }
      } else {
        // ...or go to step 3 if channel is already connected.
        OnConnectSuccess(NTF_CHANNEL);
      }
      break;

    case NTF_CHANNEL: {
        nsRefPtr<BluetoothResultHandler> res = mResultHandlerQ.ElementAt(0);
        mResultHandlerQ.RemoveElementAt(0);

        // Init, step 3: Register Core module
        nsresult rv = mProtocol->RegisterModuleCmd(
          0x01, 0x00, new InitResultHandler(this, res));
        if (NS_FAILED(rv) && res) {
          DispatchError(res, STATUS_FAIL);
        }
      }
      break;
  }
}

void
BluetoothDaemonInterface::OnConnectError(enum Channel aChannel)
{
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(!mResultHandlerQ.IsEmpty());

  switch (aChannel) {
    case NTF_CHANNEL:
      // Close command channel
      mCmdChannel->CloseSocket();
    case CMD_CHANNEL: {
        // Signal error to caller
        nsRefPtr<BluetoothResultHandler> res = mResultHandlerQ.ElementAt(0);
        mResultHandlerQ.RemoveElementAt(0);

        if (res) {
          DispatchError(res, STATUS_FAIL);
        }
      }
      break;
  }
}

void
BluetoothDaemonInterface::OnDisconnect(enum Channel aChannel)
{
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(!mResultHandlerQ.IsEmpty());

  switch (aChannel) {
    case NTF_CHANNEL:
      // Cleanup, step 4: Close command channel
      mCmdChannel->CloseSocket();
      break;
    case CMD_CHANNEL: {
        nsRefPtr<BluetoothResultHandler> res = mResultHandlerQ.ElementAt(0);
        mResultHandlerQ.RemoveElementAt(0);

        // Cleanup, step 5: Signal success to caller
        if (res) {
          res->Cleanup();
        }
      }
      break;
  }
}

void
BluetoothDaemonInterface::Init(
  BluetoothNotificationHandler* aNotificationHandler,
  BluetoothResultHandler* aRes)
{
  sNotificationHandler = aNotificationHandler;

  mResultHandlerQ.AppendElement(aRes);

  // Init, step 1: Connect command channel...
  if (mCmdChannel->GetConnectionStatus() != SOCKET_CONNECTED) {
    nsresult rv = mCmdChannel->ConnectSocket(this, mProtocol);
    if (NS_FAILED(rv)) {
      OnConnectError(CMD_CHANNEL);
    }
  } else {
    // ...or go to step 2 if channel is already connected.
    OnConnectSuccess(CMD_CHANNEL);
  }
}

class BluetoothDaemonInterface::CleanupResultHandler MOZ_FINAL
  : public BluetoothSetupResultHandler
{
public:
  CleanupResultHandler(BluetoothDaemonInterface* aInterface)
    : mInterface(aInterface)
    , mUnregisteredCoreModule(false)
  {
    MOZ_ASSERT(mInterface);
  }

  void OnError(BluetoothStatus aStatus) MOZ_OVERRIDE
  {
    Proceed();
  }

  void UnregisterModule() MOZ_OVERRIDE
  {
    Proceed();
  }

private:
  void Proceed()
  {
    MOZ_ASSERT(NS_IsMainThread());
    MOZ_ASSERT(mInterface->mProtocol);

    if (!mUnregisteredCoreModule) {
      mUnregisteredCoreModule = true;
      // Cleanup, step 2: Unregister Core module
      mInterface->mProtocol->UnregisterModuleCmd(0x01, this);
    } else {
      // Cleanup, step 3: Close notification channel
      mInterface->mNtfChannel->CloseSocket();
    }
  }

  BluetoothDaemonInterface* mInterface;
  bool mUnregisteredCoreModule;
};

void
BluetoothDaemonInterface::Cleanup(BluetoothResultHandler* aRes)
{
  sNotificationHandler = nullptr;

  mResultHandlerQ.AppendElement(aRes);

  // Cleanup, step 1: Unregister Socket module
  mProtocol->UnregisterModuleCmd(0x02, new CleanupResultHandler(this));
}

void
BluetoothDaemonInterface::Enable(BluetoothResultHandler* aRes)
{
  static_cast<BluetoothDaemonCoreModule*>(mProtocol)->EnableCmd(aRes);
}

void
BluetoothDaemonInterface::Disable(BluetoothResultHandler* aRes)
{
  static_cast<BluetoothDaemonCoreModule*>(mProtocol)->DisableCmd(aRes);
}

/* Adapter Properties */

void
BluetoothDaemonInterface::GetAdapterProperties(BluetoothResultHandler* aRes)
{
  static_cast<BluetoothDaemonCoreModule*>
    (mProtocol)->GetAdapterPropertiesCmd(aRes);
}

void
BluetoothDaemonInterface::GetAdapterProperty(const nsAString& aName,
                                             BluetoothResultHandler* aRes)
{
  static_cast<BluetoothDaemonCoreModule*>
    (mProtocol)->GetAdapterPropertyCmd(aName, aRes);
}

void
BluetoothDaemonInterface::SetAdapterProperty(
  const BluetoothNamedValue& aProperty, BluetoothResultHandler* aRes)
{
  static_cast<BluetoothDaemonCoreModule*>
    (mProtocol)->SetAdapterPropertyCmd(aProperty, aRes);
}

/* Remote Device Properties */

void
BluetoothDaemonInterface::GetRemoteDeviceProperties(
  const nsAString& aRemoteAddr, BluetoothResultHandler* aRes)
{
  static_cast<BluetoothDaemonCoreModule*>
    (mProtocol)->GetRemoteDevicePropertiesCmd(aRemoteAddr, aRes);
}

void
BluetoothDaemonInterface::GetRemoteDeviceProperty(
  const nsAString& aRemoteAddr, const nsAString& aName,
  BluetoothResultHandler* aRes)
{
  static_cast<BluetoothDaemonCoreModule*>
    (mProtocol)->GetRemoteDevicePropertyCmd(aRemoteAddr, aName, aRes);
}

void
BluetoothDaemonInterface::SetRemoteDeviceProperty(
  const nsAString& aRemoteAddr, const BluetoothNamedValue& aProperty,
  BluetoothResultHandler* aRes)
{
  static_cast<BluetoothDaemonCoreModule*>
    (mProtocol)->SetRemoteDevicePropertyCmd(aRemoteAddr, aProperty, aRes);
}

/* Remote Services */

void
BluetoothDaemonInterface::GetRemoteServiceRecord(const nsAString& aRemoteAddr,
                                                 const uint8_t aUuid[16],
                                                 BluetoothResultHandler* aRes)
{
  static_cast<BluetoothDaemonCoreModule*>(
    mProtocol)->GetRemoteServiceRecordCmd(aRemoteAddr, aUuid, aRes);
}

void
BluetoothDaemonInterface::GetRemoteServices(const nsAString& aRemoteAddr,
                                            BluetoothResultHandler* aRes)
{
  static_cast<BluetoothDaemonCoreModule*>(
    mProtocol)->GetRemoteServicesCmd(aRemoteAddr, aRes);
}

/* Discovery */

void
BluetoothDaemonInterface::StartDiscovery(BluetoothResultHandler* aRes)
{
  static_cast<BluetoothDaemonCoreModule*>(mProtocol)->StartDiscoveryCmd(aRes);
}

void
BluetoothDaemonInterface::CancelDiscovery(BluetoothResultHandler* aRes)
{
  static_cast<BluetoothDaemonCoreModule*>
    (mProtocol)->CancelDiscoveryCmd(aRes);
}

/* Bonds */

void
BluetoothDaemonInterface::CreateBond(const nsAString& aBdAddr,
                                     BluetoothResultHandler* aRes)
{
  static_cast<BluetoothDaemonCoreModule*>
    (mProtocol)->CreateBondCmd(aBdAddr, aRes);
}

void
BluetoothDaemonInterface::RemoveBond(const nsAString& aBdAddr,
                                     BluetoothResultHandler* aRes)
{
  static_cast<BluetoothDaemonCoreModule*>
    (mProtocol)->RemoveBondCmd(aBdAddr, aRes);
}

void
BluetoothDaemonInterface::CancelBond(const nsAString& aBdAddr,
                                     BluetoothResultHandler* aRes)
{
  static_cast<BluetoothDaemonCoreModule*>
    (mProtocol)->CancelBondCmd(aBdAddr, aRes);
}

/* Authentication */

void
BluetoothDaemonInterface::PinReply(const nsAString& aBdAddr, bool aAccept,
                                   const nsAString& aPinCode,
                                   BluetoothResultHandler* aRes)
{
  static_cast<BluetoothDaemonCoreModule*>
    (mProtocol)->PinReplyCmd(aBdAddr, aAccept, aPinCode, aRes);
}

void
BluetoothDaemonInterface::SspReply(const nsAString& aBdAddr,
                                   const nsAString& aVariant,
                                   bool aAccept, uint32_t aPasskey,
                                   BluetoothResultHandler* aRes)
{
  static_cast<BluetoothDaemonCoreModule*>
    (mProtocol)->SspReplyCmd(aBdAddr, aVariant, aAccept, aPasskey, aRes);
}

/* DUT Mode */

void
BluetoothDaemonInterface::DutModeConfigure(bool aEnable,
                                           BluetoothResultHandler* aRes)
{
  static_cast<BluetoothDaemonCoreModule*>
    (mProtocol)->DutModeConfigureCmd(aEnable, aRes);
}

void
BluetoothDaemonInterface::DutModeSend(uint16_t aOpcode, uint8_t* aBuf,
                                      uint8_t aLen,
                                      BluetoothResultHandler* aRes)
{
  static_cast<BluetoothDaemonCoreModule*>
    (mProtocol)->DutModeSendCmd(aOpcode, aBuf, aLen, aRes);
}

/* LE Mode */

void
BluetoothDaemonInterface::LeTestMode(uint16_t aOpcode, uint8_t* aBuf,
                                     uint8_t aLen,
                                     BluetoothResultHandler* aRes)
{
  static_cast<BluetoothDaemonCoreModule*>
    (mProtocol)->LeTestModeCmd(aOpcode, aBuf, aLen, aRes);
}

void
BluetoothDaemonInterface::DispatchError(BluetoothResultHandler* aRes,
                                        BluetoothStatus aStatus)
{
  BluetoothResultRunnable1<
    BluetoothResultHandler, void, BluetoothStatus, BluetoothStatus>::Dispatch(
    aRes, &BluetoothResultHandler::OnError,
    ConstantInitOp1<BluetoothStatus>(aStatus));
}

// Profile Interfaces
//

BluetoothSocketInterface*
BluetoothDaemonInterface::GetBluetoothSocketInterface()
{
  if (mSocketInterface) {
    return mSocketInterface;
  }

  mSocketInterface = new BluetoothDaemonSocketInterface(mProtocol);

  return mSocketInterface;
}

BluetoothHandsfreeInterface*
BluetoothDaemonInterface::GetBluetoothHandsfreeInterface()
{
  return nullptr;
}

BluetoothA2dpInterface*
BluetoothDaemonInterface::GetBluetoothA2dpInterface()
{
  return nullptr;
}

BluetoothAvrcpInterface*
BluetoothDaemonInterface::GetBluetoothAvrcpInterface()
{
  return nullptr;
}

END_BLUETOOTH_NAMESPACE
