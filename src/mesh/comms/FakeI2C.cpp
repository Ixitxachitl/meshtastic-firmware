#ifdef SENSECAP_INDICATOR

#include "FakeI2C.h"
#include "configuration.h"

FakeI2C *fakeI2C = nullptr;

FakeI2C::FakeI2C()
{
    // Initialize I2C proxy - placeholder for future functionality
    LOG_DEBUG("FakeI2C initialized");
}

#endif
