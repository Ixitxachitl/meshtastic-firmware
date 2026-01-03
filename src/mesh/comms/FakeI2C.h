#ifdef SENSECAP_INDICATOR

#pragma once

// FakeI2C will handle I2C proxy functionality in the future
class FakeI2C
{
  public:
    FakeI2C();
};

extern FakeI2C *fakeI2C;

#endif
