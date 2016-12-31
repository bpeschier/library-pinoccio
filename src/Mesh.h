#ifndef SCOUT_MESH_H_
#define SCOUT_MESH_H_

#include <Settings.h>
#include <cbor.h>
#include "lwm/nwk/nwk.h"
#include "cn-cbor/cn-cbor.h"

namespace pinoccio {
    typedef struct SendQueue_t {
        SendQueue_t* next = 0;
        uint16_t     queueId = 0;
        uint8_t*     data;
        uint16_t     dstAddr;
        uint8_t      dstEndpoint;
        uint8_t      srcEndpoint;
        uint8_t      options;
        uint8_t      size;
    };

    class Mesh {
    public:
        void setup(Settings settings);

        void loop();

        void setRadio(const uint16_t localAddress, const uint16_t panId = 0x4567, const uint8_t channel = 20);

        void setChannel(const uint8_t channel);

        void setPower(const uint8_t power);

        void setDataRate(const uint8_t rate);

        void setSecurityKey(const uint8_t *key);

        void resetSecurityKey(void);

        void listen(uint8_t endpoint, bool (*handler)(uint16_t srcAddress, uint8_t srcEndpoint, cn_cbor *data));

        void send(uint16_t address, uint8_t srcEndpoint, uint8_t dstEndpoint, cn_cbor *data, bool multicast = false);

        void multicast(uint16_t address, uint8_t srcEndpoint, uint8_t dstEndpoint, cn_cbor *data);

        void broadcast(uint8_t srcEndpoint, uint8_t dstEndpoint, cn_cbor *data);

        void join(uint16_t groupAddress);

        void leave(uint16_t groupAddress);

        bool isMemberOf(uint16_t groupAddress);

        uint16_t getAddress();

        uint16_t getPanId();

        uint8_t getChannel();

        uint8_t getTxPower();

    };
}

#endif