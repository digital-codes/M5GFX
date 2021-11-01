/*----------------------------------------------------------------------------/
  Lovyan GFX - Graphics library for embedded devices.

Original Source:
 https://github.com/lovyan03/LovyanGFX/

Licence:
 [FreeBSD](https://github.com/lovyan03/LovyanGFX/blob/master/license.txt)

Author:
 [lovyan03](https://twitter.com/lovyan03)

Contributors:
 [ciniml](https://github.com/ciniml)
 [mongonta0716](https://github.com/mongonta0716)
 [tobozo](https://github.com/tobozo)
/----------------------------------------------------------------------------*/
#if defined (ESP_PLATFORM)
#include <sdkconfig.h>
#if !defined (CONFIG_IDF_TARGET) || defined (CONFIG_IDF_TARGET_ESP32)

#include "Panel_M5HDMI.hpp"
#include "Panel_M5HDMI_FS.h"
#include "../Bus.hpp"
#include "../platforms/common.hpp"
#include "../misc/pixelcopy.hpp"
#include "../misc/colortype.hpp"

#include <esp_log.h>

#if __has_include(<alloca.h>)
#include <alloca.h>
#else
#include <malloc.h>
#define alloca _alloca
#endif

#define TAG "M5HDMI"

namespace lgfx
{
 inline namespace v1
 {

//----------------------------------------------------------------------------

  enum GWFPGA_Inst_Def
  { 
    ISC_NOOP          = 0x02,
    ISC_ERASE         = 0x05,
    ERASE_DONE        = 0x09,
    READ_ID_CODE      = 0x11,
    ISC_ENABLE        = 0x15,
    FAST_PROGRAM      = 0x17,
    STATUS_CODE       = 0x41,
    JTAG_EF_PROGRAM   = 0x71,
    JTAG_EF_READ      = 0x73,
    JTAG_EF_ERASE     = 0x75,
    ISC_DISABLE       = 0x3A,
    REPROGRAM         = 0x3C,
    Bypass            = 0xFF
  };

  enum GWFPGA_StatusReg_Def
  {
    STATUS_CRC_ERROR            = (1<<0),
    STATUS_BAD_COMMAND          = (1<<1),
    STATUS_ID_VERIFY_FAILED     = (1<<2),
    STATUS_TIMEOUT              = (1<<3),
    STATUS_MEMORY_ERASE         = (1<<5),
    STATUS_PREAMBLE             = (1<<6),
    STATUS_SYSTEM_EDIT_MODE     = (1<<7),
    STATUS_PRG_SPIFLASH_DIRECT  = (1<<8),
    STATUS_NON_JTAG_CNF_ACTIVE  = (1<<10),
    STATUS_BYPASS               = (1<<11),
    STATUS_GOWIN_VLD            = (1<<12),
    STATUS_DONE_FINAL           = (1<<13),
    STATUS_SECURITY_FINAL       = (1<<14),
    STATUS_READY                = (1<<15),
    STATUS_POR                  = (1<<16),
    STATUS_FLASH_LOCK           = (1<<17)
  };

  Panel_M5HDMI::LOAD_FPGA::LOAD_FPGA(uint_fast8_t _TCK_PIN, uint_fast8_t _TDI_PIN, uint_fast8_t _TDO_PIN, uint_fast8_t _TMS_PIN)
  {
    _tdi_reg[0] = lgfx::get_gpio_lo_reg(_TDI_PIN);
    _tdi_reg[1] = lgfx::get_gpio_hi_reg(_TDI_PIN);
    _tck_reg[0] = lgfx::get_gpio_lo_reg(_TCK_PIN);
    _tck_reg[1] = lgfx::get_gpio_hi_reg(_TCK_PIN);
    _tms_reg[0] = lgfx::get_gpio_lo_reg(_TMS_PIN);
    _tms_reg[1] = lgfx::get_gpio_hi_reg(_TMS_PIN);

    lgfx::pinMode(_TCK_PIN, lgfx::pin_mode_t::output);
    lgfx::pinMode(_TDI_PIN, lgfx::pin_mode_t::output);
    lgfx::pinMode(_TMS_PIN, lgfx::pin_mode_t::output);
    lgfx::pinMode(_TDO_PIN, lgfx::pin_mode_t::input);

    TCK_MASK = 1 << (31 & _TCK_PIN);
    TDI_MASK = 1 << (31 & _TDI_PIN);
    TMS_MASK = 1 << (31 & _TMS_PIN);
    TDO_PIN = _TDO_PIN;

    *_tms_reg[0] = TMS_MASK;
    *_tdi_reg[0] = TDI_MASK;
    *_tck_reg[0] = TCK_MASK;

    JTAG_MoveTap(TAP_UNKNOWN, TAP_IDLE);

    ESP_LOGI(TAG, "Erase FPGA SRAM...");

    JTAG_WriteInst(ISC_ENABLE);
    JTAG_WriteInst(ISC_ERASE);
    JTAG_WriteInst(ISC_NOOP);

    JTAG_DUMMY_CLOCK(4);

    JTAG_WriteInst(ERASE_DONE);
    JTAG_WriteInst(ISC_NOOP);

    JTAG_WriteInst(ISC_DISABLE);
    JTAG_WriteInst(ISC_NOOP);

    JTAG_DUMMY_CLOCK(4);

    ESP_LOGI(TAG, "Starting Writing to SRAM...");
    JTAG_WriteInst(ISC_ENABLE);
    JTAG_WriteInst(FAST_PROGRAM);
    
    JTAG_MoveTap(TAP_IDLE, TAP_DRSHIFT);

    int32_t rle_len = -1;
    int32_t direct_len = -1;
    for (size_t i = 0; i < sizeof(fs_bitstream_rle); ++i)
    {
      bool tx_end = (i == sizeof(fs_bitstream_rle) - 1);

      if (rle_len < 0)
      {
        rle_len = fs_bitstream_rle[i];
        direct_len = -1;
      }
      else
      if (rle_len == 0)
      { // direct mode
        if (direct_len == -1)
        {
          direct_len = fs_bitstream_rle[i];
        }
        else
        {
          JTAG_Write(fs_bitstream_rle[i], tx_end, 0x00);
          if (0 == --direct_len)
          {
            rle_len = -1;
          }
        }
      }
      else
      { // rle mode
        JTAG_Write(fs_bitstream_rle[i], tx_end, 0x00, rle_len);
        rle_len = -1;
      }
    }

    JTAG_MoveTap(TAP_DREXIT1,  TAP_IDLE);
    JTAG_WriteInst(ISC_DISABLE);
    JTAG_WriteInst(ISC_NOOP);

    ESP_LOGI(TAG, "SRAM Prog Finish...");
  }

  void Panel_M5HDMI::LOAD_FPGA::JTAG_DUMMY_CLOCK(uint32_t msec)
  {
    uint32_t erase_time = lgfx::millis();
    auto lo_reg = _tck_reg[0];
    auto hi_reg = _tck_reg[1];
    do
    {
      *lo_reg = TCK_MASK;
      *hi_reg = TCK_MASK;
      *lo_reg = TCK_MASK;
      *hi_reg = TCK_MASK;
    } while (lgfx::millis() - erase_time < msec);
  }

  void Panel_M5HDMI::LOAD_FPGA::JTAG_MoveTap(TAP_TypeDef TAP_From, TAP_TypeDef TAP_To)
  {
    int high = 1;
    int low = 2;
    if ((TAP_From == TAP_UNKNOWN) && (TAP_To==TAP_IDLE) )
    {
      high = 8;
    }
    else if ((TAP_From == TAP_IDLE) && (TAP_To==TAP_IDLE) )
    {
      high = 0;
      low = 3;
    }
    else if ((TAP_From == TAP_IDLE) && (TAP_To==TAP_IRSHIFT) )
    {
      high = 2;
    }
    else if ((TAP_From == TAP_IDLE) && (TAP_To==TAP_DRSHIFT) )
    {}
    else if ((TAP_From == TAP_IREXIT1) && (TAP_To==TAP_IDLE) )
    {
      low = 11;  // IREXIT1->IDLE + (IDLE->IDLE x3)
    }
    else if ((TAP_From == TAP_DREXIT1) && (TAP_To==TAP_IDLE) )
    {}
    else
    {
      ESP_LOGI(TAG, "error tap walking.");
      return;
    }

    if (high)
    {
      JTAG_TapMove_Inner(true, high);
    }
    JTAG_TapMove_Inner(false, low);
  }

  void Panel_M5HDMI::LOAD_FPGA::JTAG_TapMove_Inner(bool tms_value, size_t clock_count)
  {
    *_tms_reg[tms_value] = TMS_MASK;
    auto tck_mask = TCK_MASK;
    do
    {
      *_tck_reg[0] = tck_mask;
      *_tck_reg[1] = tck_mask;
    } while (--clock_count);
  }

  void Panel_M5HDMI::LOAD_FPGA::JTAG_Write(uint_fast8_t din, bool tms, bool LSB, size_t len)
  {
    if (LSB && din)
    {
      din = ((din & 0x55) << 1) + ((din >> 1) & 0x55);
      din = ((din & 0x33) << 2) + ((din >> 2) & 0x33);
      din = ((din & 0x0F) << 4) + ((din >> 4) & 0x0F);
    }
    *_tms_reg[0] = TMS_MASK;
    uint32_t tdi_mask = TDI_MASK;
    uint32_t tck_mask = TCK_MASK;
    uint_fast8_t prev = ~0;
    do
    {
      size_t i = 8;
      do
      {
        if (!--i && tms && (len == 1))
        {
          *_tms_reg[1] = TMS_MASK;
        }
        uint_fast8_t lv = (1 & (din >> i));
        if (prev != lv)
        {
          prev = lv;
          *_tdi_reg[lv] = tdi_mask;
        }
        *_tck_reg[0] = tck_mask;
        *_tck_reg[1] = tck_mask;
      } while (i);
    } while (--len);
  }

  void Panel_M5HDMI::LOAD_FPGA::JTAG_WriteInst(uint8_t inst)
  {
    JTAG_MoveTap(TAP_IDLE, TAP_IRSHIFT);
    JTAG_Write(inst, 0x1, true);
    JTAG_MoveTap(TAP_IREXIT1, TAP_IDLE);
  }

  uint32_t Panel_M5HDMI::LOAD_FPGA::JTAG_ReadStatus()
  {
    uint32_t out = 0;
    JTAG_WriteInst(STATUS_CODE);
    JTAG_MoveTap(TAP_IDLE, TAP_DRSHIFT);

    *_tms_reg[0] = TMS_MASK;
    for (size_t i = 0; i < 32; i++)
    {
      if (i == 31) { *_tms_reg[1] = TMS_MASK; }
      *_tck_reg[0] = TCK_MASK;
      *_tck_reg[1] = TCK_MASK;
      if (lgfx::gpio_in(TDO_PIN)) { out += 1 << i; }
    };
    JTAG_MoveTap(TAP_DREXIT1,  TAP_IDLE);
    return out;   
  }

//----------------------------------------------------------------------------

  bool Panel_M5HDMI::HDMI_Trans::writeRegister(uint8_t register_address, uint8_t value)
  {
    uint8_t buffer[2] = {
      register_address,
      value,
    };
    int retry = 4;
    while (lgfx::i2c::transactionWrite(this->HDMI_Trans_config.i2c_port, this->HDMI_Trans_config.i2c_addr, buffer, 2, this->HDMI_Trans_config.freq_write).has_error() && --retry)
    {
      lgfx::delay(1);
    }
    if (!retry)
    {
      ESP_LOGI(TAG, "i2c write err  reg:%02x val:%02x", register_address, value);
    }
    return retry != 0;
  }

  bool Panel_M5HDMI::HDMI_Trans::writeRegisterSet(const uint8_t *reg_data_pair, size_t len)
  {
    size_t idx = 0;
    do
    {
      if (!this->writeRegister(reg_data_pair[idx], reg_data_pair[idx + 1]))
      {
        return false;
      }
    } while ((idx += 2) < len);
    return true;
  }

  Panel_M5HDMI::HDMI_Trans::ChipID Panel_M5HDMI::HDMI_Trans::readChipID(void)
  {
    this->writeRegister(0xff, 0x80);
    this->writeRegister(0xee, 0x01);

    ChipID chip_id;
    chip_id.id[0] = this->readRegister(0x00);
    chip_id.id[1] = this->readRegister(0x01);
    chip_id.id[2] = this->readRegister(0x02);
    return chip_id;
  }

  void Panel_M5HDMI::HDMI_Trans::reset(void)
  {
    static constexpr const uint8_t data[] = { 0xff, 0x81, 0x30, 0x00, 0x02, 0x66, 0x0a, 0x06, 0x15, 0x06, 0x4e, 0xa8, 0xff, 0x80, 0xee, 0x01, 0x11, 0x00, 0x13, 0xf1, 0x13, 0xf9, 0x0a, 0x80, 0xff, 0x82, 0x1b, 0x77, 0x1c, 0xec, 0x45, 0x00, 0x4f, 0x40, 0x50, 0x00, 0x47, 0x07 };
    this->writeRegisterSet(data, sizeof(data));
  }

  bool Panel_M5HDMI::HDMI_Trans::init(void)
  {
    auto id = this->readChipID();
    {
      static constexpr const uint8_t data_1[] = { 0xff, 0x82, 0xde, 0x00, 0xde, 0xc0, 0xff, 0x81, 0x23, 0x40, 0x24, 0x64, 0x26, 0x55, 0x29, 0x04, 0x4d, 0x00, 0x27, 0x60, 0x28, 0x00, 0x25, 0x01, 0x2c, 0x94, 0x2d, 0x99 };
      this->writeRegisterSet(data_1, sizeof(data_1));
    }
    this->writeRegister(0x2b, this->readRegister(0x2b) & 0xfd);
    this->writeRegister(0x2e, this->readRegister(0x2e) & 0xfe);

    if ( id.id[2] == 0xE2 )
    {
      static constexpr const uint8_t data_u3[] = { 0x4d, 0x09, 0x27, 0x66, 0x28, 0x88, 0x2a, 0x00, 0x2a, 0x20, 0x25, 0x00, 0x2c, 0x9e, 0x2d, 0x99 };
      this->writeRegisterSet(data_u3, sizeof(data_u3));
    }

    for (int i = 0; i < 8; ++i)
    {
      static constexpr const uint8_t data_pll[] = { 0xff, 0x80, 0x16, 0xf1, 0x18, 0xdc, 0x18, 0xfc, 0x16, 0xf3, 0x16, 0xe3, 0x16, 0xf3, 0xff, 0x82 };
      this->writeRegisterSet(data_pll, sizeof(data_pll));
      auto locked = (this->readRegister(0x15) & 0x80) != 0;
      auto value = this->readRegister(0xea);
      auto done = (this->readRegister(0xeb) & 0x80) != 0;
      if ( locked && done && value != 0xff)
      {
        static constexpr const uint8_t data[] = { 0xb9, 0x00, 0xff, 0x84, 0x43, 0x31, 0x44, 0x10, 0x45, 0x2a, 0x47, 0x04, 0x10, 0x2c, 0x12, 0x64, 0x3d, 0x0a, 0xff, 0x80, 0x11, 0x00, 0x13, 0xf1, 0x13, 0xf9, 0xff, 0x81, 0x31, 0x44, 0x32, 0x4a, 0x33, 0x0b, 0x34, 0x00, 0x35, 0x00, 0x36, 0x00, 0x37, 0x44, 0x3f, 0x0f, 0x40, 0xa0, 0x41, 0xa0, 0x42, 0xa0, 0x43, 0xa0, 0x44, 0xa0, 0x30, 0xea };
        this->writeRegisterSet(data, sizeof(data));
        return true;
      }
    }
    ESP_LOGE(TAG, "failed to initialize the HDMI transmitter.");
    return false;
  }

//----------------------------------------------------------------------------

  bool Panel_M5HDMI::init(bool use_reset)
  {
    ESP_LOGI(TAG, "i2c port:%d sda:%d scl:%d", _HDMI_Trans_config.i2c_port, _HDMI_Trans_config.pin_sda, _HDMI_Trans_config.pin_scl);

    lgfx::i2c::init(_HDMI_Trans_config.i2c_port, _HDMI_Trans_config.pin_sda, _HDMI_Trans_config.pin_scl);

    HDMI_Trans driver(_HDMI_Trans_config);

    auto result = driver.readChipID();
    ESP_LOGI(TAG, "Chip ID: %02x %02x %02x\n", result.id[0], result.id[1], result.id[2]);
    if (result.id[0] == result.id[1] && result.id[0] == result.id[2])
    {
      lgfx::i2c::beginTransaction(_HDMI_Trans_config.i2c_port, _HDMI_Trans_config.i2c_addr, _HDMI_Trans_config.freq_write);
      lgfx::i2c::endTransaction(_HDMI_Trans_config.i2c_port);
      lgfx::delay(16);
    }

    ESP_LOGI(TAG, "Resetting HDMI transmitter...");
    driver.reset();


    {
      auto bus_cfg = reinterpret_cast<lgfx::Bus_SPI*>(bus())->config();
      auto pnl_cfg = _cfg;
      LOAD_FPGA fpga(bus_cfg.pin_sclk, bus_cfg.pin_mosi, bus_cfg.pin_miso, pnl_cfg.pin_cs);
    }


    if (!Panel_Device::init(false)) { return false; }

    ESP_LOGI(TAG, "Initialize HDMI transmitter...");
    if ( driver.init() )
    {
      ESP_LOGI(TAG, "done.");
    } 
    else
    {
      ESP_LOGI(TAG, "failed.");
    }

    // Initialize and read ID
    ESP_LOGI(TAG, "Waiting the FPGA gets idle...");
    beginTransaction();
    _bus->beginRead();
    while (_bus->readData(8) != 0xff);
    _bus->endRead();
    cs_control(true);
    ESP_LOGI(TAG, "Reading FPGA ID... ");

    cs_control(false);
    _bus->writeData(CMD_READ_ID, 8); // READ_ID
    _bus->beginRead();
    uint32_t data;
    do {
      data = _bus->readData(8);
    } while ( data == 0x00 );
    data = _bus->readData(32);
    ESP_LOGI(TAG, "FPGA ID:%02x %02x %02x %02x\n", data & 0xFF, (data >> 8) & 0xFF, (data >> 16) & 0xFF, data >> 24);
    _bus->endRead();
    endTransaction();

    uint_fast8_t x_scale = _cfg.memory_width  / _cfg.panel_width;
    uint_fast8_t y_scale = _cfg.memory_height / _cfg.panel_height;
    if (x_scale == 0) { x_scale = 1; }
    if (y_scale == 0) { y_scale = 1; }
    setScaling(x_scale, y_scale);

    return true;
  }

  void Panel_M5HDMI::beginTransaction(void)
  {
    _bus->beginTransaction();
    cs_control(false);
  }

  void Panel_M5HDMI::endTransaction(void)
  {
    _last_cmd = 0;
    _bus->wait();
    cs_control(true);
    _bus->endTransaction();
  }

  bool Panel_M5HDMI::_check_repeat(uint32_t cmd, uint_fast8_t limit)
  {
    if ((_last_cmd & ~7) == CMD_WRITE_RAW)
    {
      if (_last_cmd == cmd)
      {
        return true;
      }
      _last_cmd = cmd;

      _bus->beginRead();
      while (_bus->readData(8) == 0x00);
      cs_control(true);
      _bus->endRead();
      cs_control(false);

      return false;
    }

    _last_cmd = cmd;

    if (_need_delay)
    {
      auto us = lgfx::micros() - _last_us;
      if (_need_delay > us)
      {
        us = _need_delay - us;
        delayMicroseconds(us);
      }
      _bus->beginRead();
      while (_bus->readData(8) == 0x00)
      {
        delayMicroseconds(1);
      }
      cs_control(true);
      _bus->endRead();
      cs_control(false);
      _need_delay = 0;
    }
    return false;
  }

  color_depth_t Panel_M5HDMI::setColorDepth(color_depth_t depth)
  {
    auto bits = (depth & color_depth_t::bit_mask);
    if      (bits > 16) { depth = color_depth_t::rgb888_3Byte; }
    else if (bits < 16) { depth = color_depth_t::rgb332_1Byte; }
    else                { depth = color_depth_t::rgb565_2Byte; }

    _read_depth = _write_depth = depth;
    _read_bits  = _write_bits  = depth & color_depth_t::bit_mask;
    return _write_depth;
  }

  void Panel_M5HDMI::setRotation(uint_fast8_t r)
  {
    r &= 7;
    _rotation = r;
    _internal_rotation = ((r + _cfg.offset_rotation) & 3) | ((r & 4) ^ (_cfg.offset_rotation & 4));

    _width  = _cfg.panel_width;
    _height = _cfg.panel_height;
    if (_internal_rotation & 1) std::swap(_width, _height);
  }

  void Panel_M5HDMI::setInvert(bool invert)
  {
  }

  void Panel_M5HDMI::setSleep(bool flg)
  {
  }

  void Panel_M5HDMI::setPowerSave(bool flg)
  {
  }

  void Panel_M5HDMI::setBrightness(uint8_t brightness)
  {
  }

  void Panel_M5HDMI::writeBlock(uint32_t rawcolor, uint32_t length)
  {
    do
    {
      uint32_t h = 1;
      auto w = std::min<uint32_t>(length, _xe + 1 - _xpos);
      if (length >= (w << 1) && _xpos == _xs)
      {
        h = std::min<uint32_t>(length / w, _ye + 1 - _ypos);
      }
      writeFillRectPreclipped(_xpos, _ypos, w, h, rawcolor);
      if ((_xpos += w) <= _xe) return;
      _xpos = _xs;
      if (_ye < (_ypos += h)) { _ypos = _ys; }
      length -= w * h;
    } while (length);
  }

  void Panel_M5HDMI::drawPixelPreclipped(uint_fast16_t x, uint_fast16_t y, uint32_t rawcolor)
  {
    startWrite();
    writeFillRectPreclipped(x, y, 1, 1, rawcolor);
    endWrite();
  }

  void Panel_M5HDMI::_fill_rect(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, uint_fast8_t bytes)
  {
    bool rect = (w > 1) || (h > 1);
    uint_fast8_t r = _internal_rotation;
    if (r)
    {
      if ((1u << r) & 0b10010110) { y = _height - (y + h); }
      if (r & 2)                  { x = _width  - (x + w); }
      if (r & 1) { std::swap(x, y);  std::swap(w, h); }
    }
    uint32_t buf[4];
    buf[0] = ((rect ? CMD_FILLRECT : CMD_DRAWPIXEL) | bytes) << 24;
    bytes += 5;
    x += _cfg.offset_x;
    y += _cfg.offset_y;
    uint32_t mask = 0xFF00FF;
    uint32_t tmp = x + (y << 16);
    buf[1] = ((tmp >> 8) & mask) + ((tmp & mask) << 8);
    buf[2] = _raw_color;
    if (rect)
    {
      x += w - 1;
      y += h - 1;
      tmp = x + (y << 16);
      buf[2] = ((tmp >> 8) & mask) + ((tmp & mask) << 8);
      buf[3] = _raw_color;
      bytes += 4;
    }
    _check_repeat();
    _bus->writeBytes(((uint8_t*)buf)+3, bytes, false, false);
    if (w > 7 || h > 1)
    {
      uint32_t us = ((21 + (w >> 4) * 36 + (w & 15)) * h) >> 5;
      _need_delay = 1 + us;
      _last_us = lgfx::micros();
    }
  }

  void Panel_M5HDMI::writeFillRectPreclipped(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, uint32_t rawcolor)
  {
    size_t bytes = 0;
    if (_raw_color != rawcolor)
    {
      _raw_color = rawcolor;
      bytes = _write_bits >> 3;
    }
    _fill_rect(x, y, w, h, bytes);
  }

  void Panel_M5HDMI::writeFillRectAlphaPreclipped(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, uint32_t argb8888)
  {
    _raw_color = getSwap32(argb8888);
    _fill_rect(x, y, w, h, 4);
    _raw_color = ~0u;
  }

  void Panel_M5HDMI::setWindow(uint_fast16_t xs, uint_fast16_t ys, uint_fast16_t xe, uint_fast16_t ye)
  {
    _xpos = xs;
    _ypos = ys;
    _xs = xs;
    _ys = ys;
    _xe = xe;
    _ye = ye;
  }
  void Panel_M5HDMI::_set_window(uint_fast16_t xs, uint_fast16_t ys, uint_fast16_t xe, uint_fast16_t ye)
  {
    union cmd_t
    {
      uint8_t raw[10];
      struct __attribute__((packed))
      {
        uint8_t cmd_x;
        uint32_t data_x;
        uint8_t cmd_y;
        uint32_t data_y;
      };
    };

    static constexpr uint32_t mask = 0xFF00FF;

    cmd_t cmd;
    cmd.cmd_x = CMD_CASET;
    cmd.cmd_y = CMD_RASET;
    xs += _cfg.offset_x + ((xe + _cfg.offset_x) << 16);
    cmd.data_x = ((xs >> 8) & mask) + ((xs & mask) << 8);
    ys += _cfg.offset_y + ((ye + _cfg.offset_y) << 16);
    cmd.data_y = ((ys >> 8) & mask) + ((ys & mask) << 8);
    _check_repeat();
    _bus->writeBytes(cmd.raw, sizeof(cmd_t), false, false);
  }

  void Panel_M5HDMI::_rotate_pixelcopy(uint_fast16_t& x, uint_fast16_t& y, uint_fast16_t& w, uint_fast16_t& h, pixelcopy_t* param, uint32_t& nextx, uint32_t& nexty)
  {
    uint32_t addx = param->src_x32_add;
    uint32_t addy = param->src_y32_add;
    uint_fast8_t r = _internal_rotation;
    uint_fast8_t bitr = 1u << r;
    // if (bitr & 0b10011100)
    // {
    //   nextx = -nextx;
    // }
    if (bitr & 0b10010110) // case 1:2:4:7:
    {
      param->src_y32 += nexty * (h - 1);
      nexty = -(int32_t)nexty;
      y = _height - (y + h);
    }
    if (r & 2)
    {
      param->src_x32 += addx * (w - 1);
      param->src_y32 += addy * (w - 1);
      addx = -(int32_t)addx;
      addy = -(int32_t)addy;
      x = _width  - (x + w);
    }
    if (r & 1)
    {
      std::swap(x, y);
      std::swap(w, h);
      std::swap(nextx, addx);
      std::swap(nexty, addy);
    }
    param->src_x32_add = addx;
    param->src_y32_add = addy;
  }

  void Panel_M5HDMI::writePixels(pixelcopy_t* param, uint32_t length, bool use_dma)
  {
    uint_fast16_t xs = _xs;
    uint_fast16_t xe = _xe;
    uint_fast16_t ys = _ys;
    uint_fast16_t ye = _ye;
    uint_fast16_t x = _xpos;
    uint_fast16_t y = _ypos;
    auto bytes = (_write_bits >> 3);
    uint32_t cmd = CMD_WRITE_RAW + bytes;

    uint_fast8_t r = _internal_rotation;

    int_fast16_t ay = 1;
    if ((1u << r) & 0b10010110) { y = _height - (y + 1); ys = _height - (ys + 1); ye = _height - (ye + 1); ay = -1; }
    if (r & 2)
    {
      auto linebuf = (uint8_t*)alloca((xe - xs + 1) * bytes);

      pixelcopy_t pc((void*)linebuf, _write_depth, _write_depth);
      pc.src_x32_add = -1 << pixelcopy_t::FP_SCALE;

      x = _width  - (x + 1);
      xs = _width - (xs + 1);
      xe = _width  - (xe + 1);
      uint_fast16_t linelength;
      do
      {
        linelength = std::min<uint_fast16_t>(x - xe + 1, length);
        param->fp_copy(linebuf, 0, linelength, param);
        pc.src_x32 = (linelength - 1) << pixelcopy_t::FP_SCALE;
        if (r & 1)
        {
          _set_window(y, x - linelength + 1, y, x);
        }
        else
        {
          _set_window(x - linelength + 1, y, x, y);
        }
        _last_cmd = cmd;
        _bus->writeCommand(cmd, 8);
        _bus->writePixels(&pc, linelength);
        if ((x -= linelength) < xe)
        {
          x = xs;
          y = (y != ye) ? (y + ay) : ys;
        }
      } while (length -= linelength);
      if ((1u << r) & 0b10010110) { y = _height - (y + 1); }
      _ypos = y;
      _xpos = _width - (x + 1);
    }
    else
    {
      uint_fast16_t linelength;
      do
      {
        linelength = std::min<uint_fast16_t>(xe - x + 1, length);
        if (r & 1)
        {
          _set_window(y, x, y, x + linelength - 1);
        }
        else
        {
          _set_window(x, y, x + linelength - 1, y);
        }
        _last_cmd = cmd;
        _bus->writeCommand(cmd, 8);
        _bus->writePixels(param, linelength);

        if ((x += linelength) > xe)
        {
          x = xs;
          y = (y != ye) ? (y + ay) : ys;
        }
      } while (length -= linelength);
      if ((1u << r) & 0b10010110) { y = _height - (y + 1); }
      _ypos = y;
      _xpos = x;
    }
  }

  void Panel_M5HDMI::writeImage(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, pixelcopy_t* param, bool use_dma)
  {
    _raw_color = ~0u;
    uint32_t nextx = 0;
    uint32_t nexty = 1 << pixelcopy_t::FP_SCALE;
    auto r = _internal_rotation;
    if (r)
    {
      _rotate_pixelcopy(x, y, w, h, param, nextx, nexty);
      param->no_convert = false;
    }

    uint32_t sx32 = param->src_x32;
    uint32_t sy32 = param->src_y32;

    auto bytes = (_write_bits >> 3) & 3;
    uint32_t cmd = CMD_WRITE_RAW + bytes;

    if (param->transp == pixelcopy_t::NON_TRANSP)
    {
      _set_window(x, y, x+w-1, y+h-1);
      _bus->writeCommand(cmd, 8);
      _last_cmd = cmd;

      if (param->no_convert)
      {
        if (param->src_bitwidth == w || h == 1)
        {
          w *= h;
          h = 1;
        }
        do
        {
          uint32_t i = (param->src_x + param->src_y * param->src_bitwidth) * bytes;
          auto src = &((const uint8_t*)param->src_data)[i];
          _bus->writeBytes(src, w * bytes, false, use_dma);
          param->src_x32 = (sx32 += nextx);
          param->src_y32 = (sy32 += nexty);
        } while (--h);
      }
      else
      {
        if (w == 1 && h > 1)
        {
          param->src_x32_add = nextx;
          param->src_y32_add = nexty;
          w = h;
          h = 1;
        }
        do
        {
          _bus->writePixels(param, w);
          param->src_x32 = (sx32 += nextx);
          param->src_y32 = (sy32 += nexty);
        } while (--h);
      }
    }
    else
    {
      uint32_t wb = w * bytes;
      do
      {
        uint32_t i = 0;
        while (w != (i = param->fp_skip(i, w, param)))
        {
          auto dmabuf = _bus->getDMABuffer(wb + 1);
          dmabuf[0] = cmd;
          auto buf = &dmabuf[1];
          int32_t len = param->fp_copy(buf, 0, w - i, param);
          _set_window(x + i, y, x + i + len - 1, y);
          _last_cmd = cmd;
          _bus->writeBytes(dmabuf, 1 + (len * bytes), false, true);
          if (w == (i += len)) break;
        }
        param->src_x32 = (sx32 += nextx);
        param->src_y32 = (sy32 += nexty);
        y ++;
      } while (--h);
    }
  }

  void Panel_M5HDMI::writeImageARGB(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, pixelcopy_t* param)
  {
    // unimplemented
  }

  void Panel_M5HDMI::readRect(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, void* dst, pixelcopy_t* param)
  {
    // unimplemented
  }

  void Panel_M5HDMI::copyRect(uint_fast16_t dst_x, uint_fast16_t dst_y, uint_fast16_t w, uint_fast16_t h, uint_fast16_t src_x, uint_fast16_t src_y)
  {
    uint8_t buf[26];
    size_t idx = 0;

    src_x += _cfg.offset_x;
    dst_x += _cfg.offset_x;
    src_y += _cfg.offset_y;
    dst_y += _cfg.offset_y;

    auto xe = src_x + w - 1;
    auto ye = src_y + h - 1;

    buf[idx++] = CMD_COPYRECT;
    buf[idx++] = src_x >> 8;
    buf[idx++] = src_x;
    buf[idx++] = src_y >> 8;
    buf[idx++] = src_y;
    buf[idx++] = xe >> 8;
    buf[idx++] = xe;
    buf[idx++] = ye >> 8;
    buf[idx++] = ye;

    if (src_y <= dst_y)
    {
      buf[idx++] = src_x >> 8;
      buf[idx++] = src_x;
      buf[idx++] = (_cfg.memory_height + src_y) >> 8;
      buf[idx++] = (_cfg.memory_height + src_y);
      buf[idx++] = CMD_COPYRECT;
      buf[idx++] = src_x >> 8;
      buf[idx++] = src_x;
      buf[idx++] = (_cfg.memory_height + src_y) >> 8;
      buf[idx++] = (_cfg.memory_height + src_y);
      buf[idx++] = xe >> 8;
      buf[idx++] = xe;
      buf[idx++] = (_cfg.memory_height + ye) >> 8;
      buf[idx++] = (_cfg.memory_height + ye);
    }
    buf[idx++] = dst_x >> 8;
    buf[idx++] = dst_x;
    buf[idx++] = dst_y >> 8;
    buf[idx++] = dst_y;

    startWrite();
    _check_repeat();
    _bus->writeBytes(buf, idx, false, false);
    --w;
    _need_delay = (w + ((16 + (w >> 4) * 40 + (w & 15)) * ((h << 2) ))) >> (src_y > dst_y ? 6 : 5);
    _last_us = lgfx::micros();
    endWrite();
  }

  void Panel_M5HDMI::setScaling(uint_fast8_t x_scale, uint_fast8_t y_scale)
  {
    union cmd_t
    {
      uint8_t raw[8];
      struct __attribute__((packed))
      {
        uint8_t cmd;
        uint8_t x_scale;
        uint8_t y_scale;
        uint32_t width_height;
        uint8_t chksum;
      };
    };

    static constexpr uint32_t mask = 0xFF00FF;

    uint32_t w = _cfg.memory_width / x_scale;

    while (w * x_scale != _cfg.memory_width)
    {
      w = _cfg.memory_width / --x_scale;
    }
    uint32_t h = _cfg.memory_height / y_scale;
    uint32_t wh = w + (h << 16);

    cmd_t cmd;
    cmd.cmd = CMD_SCREEN_SCALING;
    cmd.x_scale = x_scale;
    cmd.y_scale = y_scale;
    cmd.width_height = ((wh >> 8) & mask) + ((wh & mask) << 8);

    uint_fast8_t sum = 0;
    for (size_t i = 0; i < sizeof(cmd_t)-1; ++i)
    {
      sum += cmd.raw[i];
    }
    cmd.chksum = ~sum;

    startWrite();
    _check_repeat();
    _bus->writeBytes(cmd.raw, sizeof(cmd_t), false, false);
    endWrite();
  }

  void Panel_M5HDMI::setViewPort(uint_fast16_t x, uint_fast16_t y)
  {
    union cmd_t
    {
      uint8_t raw[5];
      struct __attribute__((packed))
      {
        uint8_t cmd;
        uint32_t xy;
      };
    };
    static constexpr uint32_t mask = 0xFF00FF;

    cmd_t cmd;
    cmd.cmd = CMD_SCREEN_ORIGIN;
    uint32_t xy = x + (y << 16);
    cmd.xy = ((xy >> 8) & mask) + ((xy & mask) << 8);

    startWrite();
    _check_repeat();
    _bus->writeBytes(cmd.raw, sizeof(cmd_t), false, false);
    endWrite();
  }

//----------------------------------------------------------------------------
 }
}

#endif
#endif