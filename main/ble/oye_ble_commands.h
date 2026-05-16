#ifndef OYE_BLE_COMMANDS_H
#define OYE_BLE_COMMANDS_H

#include "oye/device/v1/device.pb.h"

namespace oye::ble {

/** Fill response envelope from request; uses bonded flag for auth-sensitive cmds. */
void HandleEnvelope(const oye_device_v1_Envelope& request, oye_device_v1_Envelope& response, bool bonded);

}  // namespace oye::ble

#endif
