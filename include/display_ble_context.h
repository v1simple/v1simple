#pragma once

// Loop-supplied snapshot keeps renderers independent of V1BLEClient.
struct DisplayBleContext {
    bool v1Connected = false;
    bool proxyConnected = false;
    int v1Rssi = 0;
    int proxyRssi = 0;
};
