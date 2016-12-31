#ifndef SCOUT_BRIDGE_H_
#define SCOUT_BRIDGE_H_

#include "Module.h"
#include <cbor.h>
#include "cn-cbor/cn-cbor.h"

#include "../Scout.h"

namespace pinoccio {
    class Bridge : public Module {
    public:
        void setup(Scout *scout);

        void loop();

        static bool handleReport(uint16_t srcAddress, uint8_t srcEndpoint, cn_cbor *data);

    private:
        using Module::Module;
    };
}

extern pinoccio::Bridge bridge;

#endif