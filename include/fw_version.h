// Firmware version, shared by the Cardputer console and the sensor nodes.
// CI and releases put it in the binary file names (see .github/workflows), and
// nodes report it to the Cardputer (10th field of the N report).
// Bump it with every firmware change you want to flash.
#pragma once
#define FW_VERSION "2.6.0-dev5"
