/*
  Created by Fabrizio Di Vittorio (fdivitto2013@gmail.com) - <http://www.fabgl.com>
  Copyright (c) 2019 Fabrizio Di Vittorio.
  All rights reserved.

  This file is part of FabGL Library.

  FabGL is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  FabGL is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with FabGL.  If not, see <http://www.gnu.org/licenses/>.
 */



#include "Arduino.h"

#include <alloca.h>
#include <stdarg.h>
#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "soc/i2s_struct.h"
#include "soc/i2s_reg.h"
#include "driver/periph_ctrl.h"
#include "rom/lldesc.h"
#include "soc/rtc.h"

#include "fabutils.h"
#include "vgacontroller.h"
#include "swgenerator.h"







namespace fabgl {





/*************************************************************************************/
/* VGAController definitions */


VGAController * VGAController::s_instance = nullptr;


VGAController::VGAController()
{
  s_instance = this;
}


void VGAController::init(gpio_num_t VSyncGPIO)
{
  m_DMABuffersHead          = nullptr;
  m_DMABuffers              = nullptr;
  m_DMABuffersVisible       = nullptr;
  m_DMABuffersCount         = 0;
  m_VSyncGPIO               = VSyncGPIO;
  m_VSyncInterruptSuspended = 1; // >0 suspended

  m_GPIOStream.begin();
}


// initializer for 8 colors configuration
void VGAController::begin(gpio_num_t redGPIO, gpio_num_t greenGPIO, gpio_num_t blueGPIO, gpio_num_t HSyncGPIO, gpio_num_t VSyncGPIO)
{
  init(VSyncGPIO);

  // GPIO configuration for bit 0
  setupGPIO(redGPIO,   VGA_RED_BIT,   GPIO_MODE_OUTPUT);
  setupGPIO(greenGPIO, VGA_GREEN_BIT, GPIO_MODE_OUTPUT);
  setupGPIO(blueGPIO,  VGA_BLUE_BIT,  GPIO_MODE_OUTPUT);

  // GPUI configuration for VSync and HSync
  setupGPIO(HSyncGPIO, VGA_HSYNC_BIT, GPIO_MODE_OUTPUT);
  setupGPIO(VSyncGPIO, VGA_VSYNC_BIT, GPIO_MODE_INPUT_OUTPUT);  // input/output so can be generated interrupt on falling/rising edge

  m_bitsPerChannel = 1;
}


// initializer for 64 colors configuration
void VGAController::begin(gpio_num_t red1GPIO, gpio_num_t red0GPIO, gpio_num_t green1GPIO, gpio_num_t green0GPIO, gpio_num_t blue1GPIO, gpio_num_t blue0GPIO, gpio_num_t HSyncGPIO, gpio_num_t VSyncGPIO)
{
  begin(red0GPIO, green0GPIO, blue0GPIO, HSyncGPIO, VSyncGPIO);

  // GPIO configuration for bit 1
  setupGPIO(red1GPIO,   VGA_RED_BIT + 1,   GPIO_MODE_OUTPUT);
  setupGPIO(green1GPIO, VGA_GREEN_BIT + 1, GPIO_MODE_OUTPUT);
  setupGPIO(blue1GPIO,  VGA_BLUE_BIT + 1,  GPIO_MODE_OUTPUT);

  m_bitsPerChannel = 2;

  // change RGB222 color conversion map to work well with Terminal
  RGB222::optimizeFor64Colors();
}


// initializer for default configuration
void VGAController::begin()
{
  begin(GPIO_NUM_22, GPIO_NUM_21, GPIO_NUM_19, GPIO_NUM_18, GPIO_NUM_5, GPIO_NUM_4, GPIO_NUM_23, GPIO_NUM_15);
}


void VGAController::setupGPIO(gpio_num_t gpio, int bit, gpio_mode_t mode)
{
  PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[gpio], PIN_FUNC_GPIO);
  gpio_set_direction(gpio, mode);
  gpio_matrix_out(gpio, I2S1O_DATA_OUT0_IDX + bit, false, false);
}


// Suspend vertical sync interrupt
// Warning: After call to suspendBackgroundPrimitiveExecution() adding primitives may cause a deadlock.
// To avoid this a call to "processPrimitives()" should be performed very often.
// Can be nested
void VGAController::suspendBackgroundPrimitiveExecution()
{
  ++m_VSyncInterruptSuspended;
  if (m_VSyncInterruptSuspended == 1)
    detachInterrupt(digitalPinToInterrupt(m_VSyncGPIO));
}


// Resume vertical sync interrupt after suspendBackgroundPrimitiveExecution()
// Can be nested
void VGAController::resumeBackgroundPrimitiveExecution()
{
  m_VSyncInterruptSuspended = tmax(0, m_VSyncInterruptSuspended - 1);
  if (m_VSyncInterruptSuspended == 0)
    attachInterrupt(digitalPinToInterrupt(m_VSyncGPIO), VSyncInterrupt, m_timings.VSyncLogic == '-' ? FALLING : RISING);
}


// modeline syntax:
//   "label" clock_mhz hdisp hsyncstart hsyncend htotal vdisp vsyncstart vsyncend vtotal (+HSync | -HSync) (+VSync | -VSync) [DoubleScan | QuadScan] [FrontPorchBegins | SyncBegins | BackPorchBegins | VisibleBegins] [MultiScanBlank]
static bool convertModelineToTimings(char const * modeline, VGATimings * timings)
{
  float freq;
  int hdisp, hsyncstart, hsyncend, htotal, vdisp, vsyncstart, vsyncend, vtotal;
  char HSyncPol = 0, VSyncPol = 0;
  int pos = 0;

  int count = sscanf(modeline, "\"%[^\"]\" %g %d %d %d %d %d %d %d %d %n", timings->label, &freq, &hdisp, &hsyncstart, &hsyncend, &htotal, &vdisp, &vsyncstart, &vsyncend, &vtotal, &pos);

  if (count == 10 && pos > 0) {

    timings->frequency      = freq * 1000000;
    timings->HVisibleArea   = hdisp;
    timings->HFrontPorch    = hsyncstart - hdisp;
    timings->HSyncPulse     = hsyncend - hsyncstart;
    timings->HBackPorch     = htotal - hsyncend;
    timings->VVisibleArea   = vdisp;
    timings->VFrontPorch    = vsyncstart - vdisp;
    timings->VSyncPulse     = vsyncend - vsyncstart;
    timings->VBackPorch     = vtotal - vsyncend;
    timings->HSyncLogic     = '-';
    timings->VSyncLogic     = '-';
    timings->scanCount      = 1;
    timings->multiScanBlack = 0;
    timings->HStartingBlock = VGAScanStart::FrontPorch;

    // get (+HSync | -HSync) (+VSync | -VSync)
    char const * pc = modeline + pos;
    for (; *pc; ++pc) {
      if (*pc == '+' || *pc == '-') {
        if (!HSyncPol)
          timings->HSyncLogic = HSyncPol = *pc;
        else if (!VSyncPol) {
          timings->VSyncLogic = VSyncPol = *pc;
          while (*pc && *pc != ' ')
            ++pc;
          break;
        }
      }
    }

    // get [DoubleScan | QuadScan] [FrontPorchBegins | SyncBegins | BackPorchBegins | VisibleBegins] [MultiScanBlank]
    // actually this gets only the first character
    while (*pc) {
      switch (*pc) {
        case 'D':
        case 'd':
          timings->scanCount = 2;
          break;
        case 'Q':
        case 'q':
          timings->scanCount = 4;
          break;
        case 'F':
        case 'f':
          timings->HStartingBlock = VGAScanStart::FrontPorch;
          break;
        case 'S':
        case 's':
          timings->HStartingBlock = VGAScanStart::Sync;
          break;
        case 'B':
        case 'b':
          timings->HStartingBlock = VGAScanStart::BackPorch;
          break;
        case 'V':
        case 'v':
          timings->HStartingBlock = VGAScanStart::VisibleArea;
          break;
        case 'M':
        case 'm':
          timings->multiScanBlack = 1;
          break;
        case ' ':
          ++pc;
          continue;
      }
      ++pc;
      while (*pc && *pc != ' ')
        ++pc;
    }

    return true;

  }
  return false;
}


void VGAController::setResolution(char const * modeline, int viewPortWidth, int viewPortHeight, bool doubleBuffered)
{
  VGATimings timings;
  if (convertModelineToTimings(modeline, &timings))
    setResolution(timings, viewPortWidth, viewPortHeight, doubleBuffered);
}


// this method may adjust m_viewPortHeight to the actual number of allocated rows.
// to reduce memory allocation overhead try to allocate the minimum number of blocks.
void VGAController::allocateViewPort()
{
  int linesCount[FABGLIB_VIEWPORT_MEMORY_POOL_COUNT]; // where store number of lines for each pool
  int poolsCount = 0; // number of allocated pools
  int remainingLines = m_viewPortHeight;
  m_viewPortHeight = 0; // m_viewPortHeight needs to be recalculated

  if (isDoubleBuffered())
    remainingLines *= 2;

  // allocate pools
  while (remainingLines > 0 && poolsCount < FABGLIB_VIEWPORT_MEMORY_POOL_COUNT) {
    int largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    linesCount[poolsCount] = tmin(remainingLines, largestBlock / m_viewPortWidth);
    if (linesCount[poolsCount] == 0)  // no more memory available for lines
      break;
    m_viewPortMemoryPool[poolsCount] = (uint8_t*) heap_caps_malloc(linesCount[poolsCount] * m_viewPortWidth, MALLOC_CAP_DMA);
    remainingLines -= linesCount[poolsCount];
    m_viewPortHeight += linesCount[poolsCount];
    ++poolsCount;
  }
  m_viewPortMemoryPool[poolsCount] = nullptr;

  // fill m_viewPort[] with line pointers
  if (isDoubleBuffered()) {
    m_viewPortHeight /= 2;
    m_viewPortVisible = (volatile uint8_t * *) heap_caps_malloc(sizeof(uint8_t*) * m_viewPortHeight, MALLOC_CAP_32BIT);
  }
  m_viewPort = (volatile uint8_t * *) heap_caps_malloc(sizeof(uint8_t*) * m_viewPortHeight, MALLOC_CAP_32BIT);
  for (int p = 0, l = 0; p < poolsCount; ++p) {
    uint8_t * pool = m_viewPortMemoryPool[p];
    for (int i = 0; i < linesCount[p]; ++i) {
      if (l + i < m_viewPortHeight)
        m_viewPort[l + i] = pool;
      else
        m_viewPortVisible[l + i - m_viewPortHeight] = pool; // set only when double buffered is enabled
      pool += m_viewPortWidth;
    }
    l += linesCount[p];
  }
}


void VGAController::freeViewPort()
{
  for (uint8_t * * poolPtr = m_viewPortMemoryPool; *poolPtr; ++poolPtr) {
    heap_caps_free((void*) *poolPtr);
    *poolPtr = nullptr;
  }
  heap_caps_free(m_viewPort);
  if (isDoubleBuffered())
    heap_caps_free(m_viewPortVisible);
}


void VGAController::setResolution(VGATimings const& timings, int viewPortWidth, int viewPortHeight, bool doubleBuffered)
{
  if (m_DMABuffers) {
    suspendBackgroundPrimitiveExecution();
    m_GPIOStream.stop();
    freeBuffers();
  }

  m_timings = timings;
  setDoubleBuffered(doubleBuffered);

  m_HLineSize = m_timings.HFrontPorch + m_timings.HSyncPulse + m_timings.HBackPorch + m_timings.HVisibleArea;

  m_HBlankLine_withVSync = (uint8_t*) heap_caps_malloc(m_HLineSize, MALLOC_CAP_DMA);
  m_HBlankLine           = (uint8_t*) heap_caps_malloc(m_HLineSize, MALLOC_CAP_DMA);

  m_viewPortWidth  = ~3 & (viewPortWidth <= 0 || viewPortWidth >= m_timings.HVisibleArea ? m_timings.HVisibleArea : viewPortWidth); // view port width must be 32 bit aligned
  m_viewPortHeight = viewPortHeight <= 0 || viewPortHeight >= m_timings.VVisibleArea ? m_timings.VVisibleArea : viewPortHeight;

  // need to center viewport?
  m_viewPortCol = (m_timings.HVisibleArea - m_viewPortWidth) / 2;
  m_viewPortRow = (m_timings.VVisibleArea - m_viewPortHeight) / 2;

  // view port col and row must be 32 bit aligned
  m_viewPortCol = m_viewPortCol & ~3;
  m_viewPortRow = m_viewPortRow & ~3;

  m_linesCount = m_timings.VVisibleArea + m_timings.VFrontPorch + m_timings.VSyncPulse + m_timings.VBackPorch;

  // allocate DMA descriptors
  setDMABuffersCount(calcRequiredDMABuffersCount(m_viewPortHeight));

  // allocate the viewport
  allocateViewPort();

  // this may free space if m_viewPortHeight has been reduced
  setDMABuffersCount(calcRequiredDMABuffersCount(m_viewPortHeight));

  // fill buffers
  fillVertBuffers(0);
  fillHorizBuffers(0);

  // fill view port
  for (int i = 0; i < m_viewPortHeight; ++i)
    fill(m_viewPort[i], 0, m_viewPortWidth, 0, 0, 0, false, false);

  m_DMABuffersHead->qe.stqe_next = (lldesc_t*) &m_DMABuffersVisible[0];

  //// set initial paint state
  m_paintState.penColor              = RGB222(3, 3, 3);
  m_paintState.brushColor            = RGB222(0, 0, 0);
  m_paintState.position              = Point(0, 0);
  m_paintState.glyphOptions.value    = 0;  // all options: 0
  m_paintState.paintOptions          = PaintOptions();
  m_paintState.scrollingRegion       = Rect(0, 0, m_viewPortWidth - 1, m_viewPortHeight - 1);
  m_paintState.origin                = Point(0, 0);
  m_paintState.clippingRect          = Rect(0, 0, m_viewPortWidth - 1, m_viewPortHeight - 1);
  m_paintState.absClippingRect       = m_paintState.clippingRect;

  // number of microseconds usable in VSynch ISR
  m_maxVSyncISRTime = ceil(1000000.0 / m_timings.frequency * m_timings.scanCount * m_HLineSize * (m_timings.VSyncPulse + m_timings.VBackPorch + m_viewPortRow));

  m_GPIOStream.play(m_timings.frequency, m_DMABuffers);
  resumeBackgroundPrimitiveExecution();
}


void VGAController::freeBuffers()
{
  if (m_DMABuffersCount > 0) {
    heap_caps_free((void*)m_HBlankLine_withVSync);
    heap_caps_free((void*)m_HBlankLine);

    freeViewPort();

    setDMABuffersCount(0);
  }
}


int VGAController::calcRequiredDMABuffersCount(int viewPortHeight)
{
  int rightPadSize = m_timings.HVisibleArea - m_viewPortWidth - m_viewPortCol;
  int buffersCount = m_timings.scanCount * (m_linesCount + viewPortHeight);

  switch (m_timings.HStartingBlock) {
    case VGAScanStart::FrontPorch:
      // FRONTPORCH -> SYNC -> BACKPORCH -> VISIBLEAREA
      buffersCount += m_timings.scanCount * (rightPadSize > 0 ? viewPortHeight : 0);
      break;
    case VGAScanStart::Sync:
      // SYNC -> BACKPORCH -> VISIBLEAREA -> FRONTPORCH
      buffersCount += m_timings.scanCount * viewPortHeight;
      break;
    case VGAScanStart::BackPorch:
      // BACKPORCH -> VISIBLEAREA -> FRONTPORCH -> SYNC
      buffersCount += m_timings.scanCount * viewPortHeight;
      break;
    case VGAScanStart::VisibleArea:
      // VISIBLEAREA -> FRONTPORCH -> SYNC -> BACKPORCH
      buffersCount += m_timings.scanCount * (m_viewPortCol > 0 ? viewPortHeight : 0);
      break;
  }

  return buffersCount;
}


void VGAController::fillVertBuffers(int offsetY)
{
  int16_t porchSum = m_timings.VFrontPorch + m_timings.VBackPorch;
  m_timings.VFrontPorch = tmax(1, (int16_t)m_timings.VFrontPorch - offsetY);
  m_timings.VBackPorch  = tmax(1, porchSum - m_timings.VFrontPorch);
  m_timings.VFrontPorch = porchSum - m_timings.VBackPorch;

  // associate buffers pointer to DMA info buffers
  //
  //  Vertical order:
  //    VisibleArea
  //    Front Porch
  //    Sync
  //    Back Porch

  int VVisibleArea_pos = 0;
  int VFrontPorch_pos  = VVisibleArea_pos + m_timings.VVisibleArea;
  int VSync_pos        = VFrontPorch_pos  + m_timings.VFrontPorch;
  int VBackPorch_pos   = VSync_pos        + m_timings.VSyncPulse;

  int rightPadSize = m_timings.HVisibleArea - m_viewPortWidth - m_viewPortCol;

  for (int line = 0, DMABufIdx = 0; line < m_linesCount; ++line) {

    bool isVVisibleArea = (line < VFrontPorch_pos);
    bool isVFrontPorch  = (line >= VFrontPorch_pos && line < VSync_pos);
    bool isVSync        = (line >= VSync_pos && line < VBackPorch_pos);
    bool isVBackPorch   = (line >= VBackPorch_pos);

    for (int scan = 0; scan < m_timings.scanCount; ++scan) {

      if (isVSync) {

        setDMABufferBlank(DMABufIdx++, m_HBlankLine_withVSync, m_HLineSize);

      } else if (isVFrontPorch || isVBackPorch) {

        setDMABufferBlank(DMABufIdx++, m_HBlankLine, m_HLineSize);

      } else if (isVVisibleArea) {

        int visibleAreaLine = line - VVisibleArea_pos;
        bool isViewport = visibleAreaLine >= m_viewPortRow && visibleAreaLine < m_viewPortRow + m_viewPortHeight;
        int HInvisibleAreaSize = m_HLineSize - m_timings.HVisibleArea;

        if (isViewport) {
          // visible, this is the viewport

          switch (m_timings.HStartingBlock) {
            case VGAScanStart::FrontPorch:
              // FRONTPORCH -> SYNC -> BACKPORCH -> VISIBLEAREA
              setDMABufferBlank(DMABufIdx++, m_HBlankLine, HInvisibleAreaSize + m_viewPortCol);
              setDMABufferView(DMABufIdx++, visibleAreaLine - m_viewPortRow, scan);
              if (rightPadSize > 0)
                setDMABufferBlank(DMABufIdx++, m_HBlankLine + HInvisibleAreaSize, rightPadSize);
              break;
            case VGAScanStart::Sync:
              // SYNC -> BACKPORCH -> VISIBLEAREA -> FRONTPORCH
              setDMABufferBlank(DMABufIdx++, m_HBlankLine, m_timings.HSyncPulse + m_timings.HBackPorch + m_viewPortCol);
              setDMABufferView(DMABufIdx++, visibleAreaLine - m_viewPortRow, scan);
              setDMABufferBlank(DMABufIdx++, m_HBlankLine + m_HLineSize - m_timings.HFrontPorch - rightPadSize, m_timings.HFrontPorch + rightPadSize);
              break;
            case VGAScanStart::BackPorch:
              // BACKPORCH -> VISIBLEAREA -> FRONTPORCH -> SYNC
              setDMABufferBlank(DMABufIdx++, m_HBlankLine, m_timings.HBackPorch + m_viewPortCol);
              setDMABufferView(DMABufIdx++, visibleAreaLine - m_viewPortRow, scan);
              setDMABufferBlank(DMABufIdx++, m_HBlankLine + m_HLineSize - m_timings.HFrontPorch - m_timings.HSyncPulse - rightPadSize, m_timings.HFrontPorch + m_timings.HSyncPulse + rightPadSize);
              break;
            case VGAScanStart::VisibleArea:
              // VISIBLEAREA -> FRONTPORCH -> SYNC -> BACKPORCH
              if (m_viewPortCol > 0)
                setDMABufferBlank(DMABufIdx++, m_HBlankLine, m_viewPortCol);
              setDMABufferView(DMABufIdx++, visibleAreaLine - m_viewPortRow, scan);
              setDMABufferBlank(DMABufIdx++, m_HBlankLine + m_timings.HVisibleArea - rightPadSize, HInvisibleAreaSize + rightPadSize);
              break;
          }

        } else {
          // not visible
          setDMABufferBlank(DMABufIdx++, m_HBlankLine, m_HLineSize);
        }

      }

    }

  }
}


// refill buffers changing Front Porch and Back Porch
// offsetX : (< 0 : left  > 0 right)
void VGAController::fillHorizBuffers(int offsetX)
{
  // fill all with no hsync

  fill(m_HBlankLine,           0, m_HLineSize, 0, 0, 0, false, false);
  fill(m_HBlankLine_withVSync, 0, m_HLineSize, 0, 0, 0, false,  true);

  // calculate hsync pos and fill it

  int16_t porchSum = m_timings.HFrontPorch + m_timings.HBackPorch;
  m_timings.HFrontPorch = tmax(8, (int16_t)m_timings.HFrontPorch - offsetX);
  m_timings.HBackPorch  = tmax(8, porchSum - m_timings.HFrontPorch);
  m_timings.HFrontPorch = porchSum - m_timings.HBackPorch;

  int syncPos = 0;

  switch (m_timings.HStartingBlock) {
    case VGAScanStart::FrontPorch:
      // FRONTPORCH -> SYNC -> BACKPORCH -> VISIBLEAREA
      syncPos = m_timings.HFrontPorch;
      break;
    case VGAScanStart::Sync:
      // SYNC -> BACKPORCH -> VISIBLEAREA -> FRONTPORCH
      syncPos = 0;
      break;
    case VGAScanStart::BackPorch:
      // BACKPORCH -> VISIBLEAREA -> FRONTPORCH -> SYNC
      syncPos = m_timings.HBackPorch + m_timings.HVisibleArea + m_timings.HFrontPorch;
      break;
    case VGAScanStart::VisibleArea:
      // VISIBLEAREA -> FRONTPORCH -> SYNC -> BACKPORCH
      syncPos = m_timings.HVisibleArea + m_timings.HFrontPorch;
      break;
  }

  fill(m_HBlankLine,           syncPos, m_timings.HSyncPulse, 0, 0, 0, true,  false);
  fill(m_HBlankLine_withVSync, syncPos, m_timings.HSyncPulse, 0, 0, 0, true,   true);
}


void VGAController::moveScreen(int offsetX, int offsetY)
{
  suspendBackgroundPrimitiveExecution();
  fillVertBuffers(offsetY);
  fillHorizBuffers(offsetX);
  resumeBackgroundPrimitiveExecution();
}


void VGAController::shrinkScreen(int shrinkX, int shrinkY)
{
  VGATimings * currTimings = getResolutionTimings();

  currTimings->HBackPorch  = tmax(currTimings->HBackPorch + 4 * shrinkX, 4);
  currTimings->HFrontPorch = tmax(currTimings->HFrontPorch + 4 * shrinkX, 4);

  currTimings->VBackPorch  = tmax(currTimings->VBackPorch + shrinkY, 1);
  currTimings->VFrontPorch = tmax(currTimings->VFrontPorch + shrinkY, 1);

  setResolution(*currTimings, m_viewPortWidth, m_viewPortHeight, isDoubleBuffered());
}


// Can be used to change buffers count, maintainig already set pointers.
bool VGAController::setDMABuffersCount(int buffersCount)
{
  if (buffersCount == 0) {
    heap_caps_free( (void*) m_DMABuffers );
    if (isDoubleBuffered())
      heap_caps_free( (void*) m_DMABuffersVisible );
    m_DMABuffers = nullptr;
    m_DMABuffersVisible = nullptr;
    m_DMABuffersCount = 0;
    return true;
  }

  if (buffersCount != m_DMABuffersCount) {

    // buffers head
    if (m_DMABuffersHead == nullptr) {
      m_DMABuffersHead = (lldesc_t*) heap_caps_malloc(sizeof(lldesc_t), MALLOC_CAP_DMA);
      m_DMABuffersHead->eof    = m_DMABuffersHead->sosf = m_DMABuffersHead->offset = 0;
      m_DMABuffersHead->owner  = 1;
      m_DMABuffersHead->size   = 0;
      m_DMABuffersHead->length = 0;
      m_DMABuffersHead->buf    = m_HBlankLine;  // dummy valid address. Setting nullptr crashes DMA!
      m_DMABuffersHead->qe.stqe_next = nullptr;  // this will be set before the first frame
    }

    // (re)allocate and initialize DMA descs
    m_DMABuffers = (lldesc_t*) heap_caps_realloc((void*)m_DMABuffers, buffersCount * sizeof(lldesc_t), MALLOC_CAP_DMA);
    if (isDoubleBuffered())
      m_DMABuffersVisible = (lldesc_t*) heap_caps_realloc((void*)m_DMABuffersVisible, buffersCount * sizeof(lldesc_t), MALLOC_CAP_DMA);
    else
      m_DMABuffersVisible = m_DMABuffers;
    if (!m_DMABuffers || !m_DMABuffersVisible)
      return false;

    for (int i = 0; i < buffersCount; ++i) {
      m_DMABuffers[i].eof          = m_DMABuffers[i].sosf = m_DMABuffers[i].offset = 0;
      m_DMABuffers[i].owner        = 1;
      m_DMABuffers[i].qe.stqe_next = (lldesc_t*) (i == buffersCount - 1 ? m_DMABuffersHead : &m_DMABuffers[i + 1]);
      if (isDoubleBuffered()) {
        m_DMABuffersVisible[i].eof          = m_DMABuffersVisible[i].sosf = m_DMABuffersVisible[i].offset = 0;
        m_DMABuffersVisible[i].owner        = 1;
        m_DMABuffersVisible[i].qe.stqe_next = (lldesc_t*) (i == buffersCount - 1 ? m_DMABuffersHead : &m_DMABuffersVisible[i + 1]);
      }
    }

    m_DMABuffersCount = buffersCount;
  }

  return true;
}


// address must be allocated with MALLOC_CAP_DMA or even an address of another already allocated buffer
// allocated buffer length (in bytes) must be 32 bit aligned
// Max length is 4092 bytes
void VGAController::setDMABufferBlank(int index, void volatile * address, int length)
{
  int size = (length + 3) & (~3);
  m_DMABuffers[index].size   = size;
  m_DMABuffers[index].length = length;
  m_DMABuffers[index].buf    = (uint8_t*) address;
  if (isDoubleBuffered()) {
    m_DMABuffersVisible[index].size   = size;
    m_DMABuffersVisible[index].length = length;
    m_DMABuffersVisible[index].buf    = (uint8_t*) address;
  }
}


// address must be allocated with MALLOC_CAP_DMA or even an address of another already allocated buffer
// allocated buffer length (in bytes) must be 32 bit aligned
// Max length is 4092 bytes
void VGAController::setDMABufferView(int index, int row, int scan, volatile uint8_t * * viewPort, bool onVisibleDMA)
{
  uint8_t * bufferPtr;
  if (scan > 0 && m_timings.multiScanBlack == 1 && m_timings.HStartingBlock == FrontPorch)
    bufferPtr = (uint8_t *) (m_HBlankLine + m_HLineSize - m_timings.HVisibleArea);  // this works only when HSYNC, FrontPorch and BackPorch are at the beginning of m_HBlankLine
  else
    bufferPtr = (uint8_t *) viewPort[row];
  lldesc_t volatile * DMABuffers = onVisibleDMA ? m_DMABuffersVisible : m_DMABuffers;
  DMABuffers[index].size   = (m_viewPortWidth + 3) & (~3);
  DMABuffers[index].length = m_viewPortWidth;
  DMABuffers[index].buf    = bufferPtr;
}


// address must be allocated with MALLOC_CAP_DMA or even an address of another already allocated buffer
// allocated buffer length (in bytes) must be 32 bit aligned
// Max length is 4092 bytes
void VGAController::setDMABufferView(int index, int row, int scan)
{
  setDMABufferView(index, row, scan, m_viewPort, false);
  if (isDoubleBuffered())
    setDMABufferView(index, row, scan, m_viewPortVisible, true);
}



void volatile * VGAController::getDMABuffer(int index, int * length)
{
  *length = m_DMABuffers[index].length;
  return m_DMABuffers[index].buf;
}


uint8_t IRAM_ATTR VGAController::packHVSync(bool HSync, bool VSync)
{
  uint8_t hsync_value = (m_timings.HSyncLogic == '+' ? (HSync ? 1 : 0) : (HSync ? 0 : 1));
  uint8_t vsync_value = (m_timings.VSyncLogic == '+' ? (VSync ? 1 : 0) : (VSync ? 0 : 1));
  return (vsync_value << VGA_VSYNC_BIT) | (hsync_value << VGA_HSYNC_BIT);
}


uint8_t IRAM_ATTR VGAController::preparePixel(RGB222 rgb, bool HSync, bool VSync)
{
  return packHVSync(HSync, VSync) | (rgb.B << VGA_BLUE_BIT) | (rgb.G << VGA_GREEN_BIT) | (rgb.R << VGA_RED_BIT);
}


// buffer: buffer to fill (buffer size must be 32 bit aligned)
// startPos: initial position (in pixels)
// length: number of pixels to fill
//
// Returns next pos to fill (startPos + length)
int VGAController::fill(uint8_t volatile * buffer, int startPos, int length, uint8_t red, uint8_t green, uint8_t blue, bool HSync, bool VSync)
{
  uint8_t pattern = preparePixel((RGB222){red, green, blue}, HSync, VSync);
  for (int i = 0; i < length; ++i, ++startPos)
    VGA_PIXELINROW(buffer, startPos) = pattern;
  return startPos;
}


void IRAM_ATTR VGAController::VSyncInterrupt()
{
  auto VGACtrl = VGAController::instance();
  int64_t startTime = VGACtrl->backgroundPrimitiveTimeoutEnabled() ? esp_timer_get_time() : 0;
  bool isFirst = true;
  do {
    Primitive prim;
    if (VGACtrl->getPrimitiveISR(&prim) == false)
      break;

    if (prim.cmd == PrimitiveCmd::SwapBuffers && !isFirst) {
      // SwapBuffers must be the first primitive executed at VSync. If not reinsert it and break execution to wait for next VSync.
      VGACtrl->insertPrimitiveISR(&prim);
      break;
    }

    VGACtrl->execPrimitive(prim);

    isFirst = false;
  } while (!VGACtrl->backgroundPrimitiveTimeoutEnabled() || (startTime + VGACtrl->m_maxVSyncISRTime > esp_timer_get_time()));
  VGACtrl->showSprites();
}


void IRAM_ATTR VGAController::execPrimitive(Primitive const & prim)
{
  switch (prim.cmd) {
    case PrimitiveCmd::SetPenColor:
      m_paintState.penColor = prim.color;
      break;
    case PrimitiveCmd::SetBrushColor:
      m_paintState.brushColor = prim.color;
      break;
    case PrimitiveCmd::SetPixel:
      execSetPixel(prim.position);
      break;
    case PrimitiveCmd::SetPixelAt:
      execSetPixelAt(prim.pixelDesc);
      break;
    case PrimitiveCmd::MoveTo:
      m_paintState.position = Point(prim.position.X + m_paintState.origin.X, prim.position.Y + m_paintState.origin.Y);
      break;
    case PrimitiveCmd::LineTo:
      execLineTo(prim.position);
      break;
    case PrimitiveCmd::FillRect:
      execFillRect(prim.rect);
      break;
    case PrimitiveCmd::DrawRect:
      execDrawRect(prim.rect);
      break;
    case PrimitiveCmd::FillEllipse:
      execFillEllipse(prim.size);
      break;
    case PrimitiveCmd::DrawEllipse:
      execDrawEllipse(prim.size);
      break;
    case PrimitiveCmd::Clear:
      execClear();
      break;
    case PrimitiveCmd::VScroll:
      execVScroll(prim.ivalue);
      break;
    case PrimitiveCmd::HScroll:
      execHScroll(prim.ivalue);
      break;
    case PrimitiveCmd::DrawGlyph:
      execDrawGlyph(prim.glyph, m_paintState.glyphOptions, m_paintState.penColor, m_paintState.brushColor);
      break;
    case PrimitiveCmd::SetGlyphOptions:
      m_paintState.glyphOptions = prim.glyphOptions;
      break;
    case PrimitiveCmd::SetPaintOptions:
      m_paintState.paintOptions = prim.paintOptions;
      break;
    case PrimitiveCmd::InvertRect:
      execInvertRect(prim.rect);
      break;
    case PrimitiveCmd::CopyRect:
      execCopyRect(prim.rect);
      break;
    case PrimitiveCmd::SetScrollingRegion:
      m_paintState.scrollingRegion = prim.rect;
      break;
    case PrimitiveCmd::SwapFGBG:
      execSwapFGBG(prim.rect);
      break;
    case PrimitiveCmd::RenderGlyphsBuffer:
      execRenderGlyphsBuffer(prim.glyphsBufferRenderInfo);
      break;
    case PrimitiveCmd::DrawBitmap:
      execDrawBitmap(prim.bitmapDrawingInfo);
      break;
    case PrimitiveCmd::RefreshSprites:
      hideSprites();
      showSprites();
      break;
    case PrimitiveCmd::SwapBuffers:
      execSwapBuffers();
      break;
    case PrimitiveCmd::DrawPath:
      execDrawPath(prim.path);
      break;
    case PrimitiveCmd::FillPath:
      execFillPath(prim.path);
      break;
    case PrimitiveCmd::SetOrigin:
      m_paintState.origin = prim.position;
      updateAbsoluteClippingRect();
      break;
    case PrimitiveCmd::SetClippingRect:
      m_paintState.clippingRect = prim.rect;
      updateAbsoluteClippingRect();
      break;
  }
}


void VGAController::updateAbsoluteClippingRect()
{
  int X1 = iclamp(m_paintState.origin.X + m_paintState.clippingRect.X1, 0, m_viewPortWidth - 1);
  int Y1 = iclamp(m_paintState.origin.Y + m_paintState.clippingRect.Y1, 0, m_viewPortHeight - 1);
  int X2 = iclamp(m_paintState.origin.X + m_paintState.clippingRect.X2, 0, m_viewPortWidth - 1);
  int Y2 = iclamp(m_paintState.origin.Y + m_paintState.clippingRect.Y2, 0, m_viewPortHeight - 1);
  m_paintState.absClippingRect = Rect(X1, Y1, X2, Y2);
}


void IRAM_ATTR VGAController::execSetPixel(Point const & position)
{
  hideSprites();
  uint8_t pattern = m_paintState.paintOptions.swapFGBG ? preparePixel(m_paintState.brushColor) : preparePixel(m_paintState.penColor);

  const int x = position.X + m_paintState.origin.X;
  const int y = position.Y + m_paintState.origin.Y;

  const int clipX1 = m_paintState.absClippingRect.X1;
  const int clipY1 = m_paintState.absClippingRect.Y1;
  const int clipX2 = m_paintState.absClippingRect.X2;
  const int clipY2 = m_paintState.absClippingRect.Y2;

  if (x >= clipX1 && x <= clipX2 && y >= clipY1 && y <= clipY2)
    VGA_PIXEL(x, y) = pattern;
}


void IRAM_ATTR VGAController::execSetPixelAt(PixelDesc const & pixelDesc)
{
  hideSprites();
  uint8_t pattern = preparePixel(pixelDesc.color);

  const int x = pixelDesc.pos.X + m_paintState.origin.X;
  const int y = pixelDesc.pos.Y + m_paintState.origin.Y;

  const int clipX1 = m_paintState.absClippingRect.X1;
  const int clipY1 = m_paintState.absClippingRect.Y1;
  const int clipX2 = m_paintState.absClippingRect.X2;
  const int clipY2 = m_paintState.absClippingRect.Y2;

  if (x >= clipX1 && x <= clipX2 && y >= clipY1 && y <= clipY2)
    VGA_PIXEL(x, y) = pattern;
}


void IRAM_ATTR VGAController::execLineTo(Point const & position)
{
  hideSprites();
  uint8_t pattern = m_paintState.paintOptions.swapFGBG ? preparePixel(m_paintState.brushColor) : preparePixel(m_paintState.penColor);

  int origX = m_paintState.origin.X;
  int origY = m_paintState.origin.Y;

  drawLine(m_paintState.position.X, m_paintState.position.Y, position.X + origX, position.Y + origY, pattern);

  m_paintState.position = Point(position.X + origX, position.Y + origY);
}


// coordinates are absolute values (not relative to origin)
// line clipped on current absolute clipping rectangle
void IRAM_ATTR VGAController::drawLine(int X1, int Y1, int X2, int Y2, uint8_t pattern)
{
  if (Y1 == Y2) {
    // horizontal line
    if (Y1 < m_paintState.absClippingRect.Y1 || Y1 > m_paintState.absClippingRect.Y2)
      return;
    if (X1 > X2)
      tswap(X1, X2);
    if (X1 > m_paintState.absClippingRect.X2 || X2 < m_paintState.absClippingRect.X1)
      return;
    X1 = iclamp(X1, m_paintState.absClippingRect.X1, m_paintState.absClippingRect.X2);
    X2 = iclamp(X2, m_paintState.absClippingRect.X1, m_paintState.absClippingRect.X2);
    uint8_t volatile * row = m_viewPort[Y1];
    if (m_paintState.paintOptions.NOT) {
      const uint8_t HVSync = packHVSync();
      for (int x = X1; x <= X2; ++x) {
        uint8_t * px = (uint8_t*) &VGA_PIXELINROW(row, x);
        *px = HVSync | ~(*px);
      }
    } else {
      for (int x = X1; x <= X2; ++x)
        VGA_PIXELINROW(row, x) = pattern;
    }
  } else if (X1 == X2) {
    // vertical line
    if (X1 < m_paintState.absClippingRect.X1 || X1 > m_paintState.absClippingRect.X2)
      return;
    if (Y1 > Y2)
      tswap(Y1, Y2);
    if (Y1 > m_paintState.absClippingRect.Y2 || Y2 < m_paintState.absClippingRect.Y1)
      return;
    Y1 = iclamp(Y1, m_paintState.absClippingRect.Y1, m_paintState.absClippingRect.Y2);
    Y2 = iclamp(Y2, m_paintState.absClippingRect.Y1, m_paintState.absClippingRect.Y2);
    if (m_paintState.paintOptions.NOT) {
      const uint8_t HVSync = packHVSync();
      for (int y = Y1; y <= Y2; ++y) {
        uint8_t * px = (uint8_t*) &VGA_PIXEL(X1, y);
        *px = HVSync | ~(*px);
      }
    } else {
      for (int y = Y1; y <= Y2; ++y)
        VGA_PIXEL(X1, y) = pattern;
    }
  } else {
    // other cases (Bresenham's algorithm)
    // TODO: to optimize
    //   Unfortunately here we cannot clip exactly using Sutherland-Cohen algorithm (as done before)
    //   because the starting line (got from clipping algorithm) may not be the same of Bresenham's
    //   line (think to continuing an existing line).
    //   Possible solutions:
    //      - "Yevgeny P. Kuzmin" algorithm:
    //               https://stackoverflow.com/questions/40884680/how-to-use-bresenhams-line-drawing-algorithm-with-clipping
    //               https://github.com/ktfh/ClippedLine/blob/master/clip.hpp
    // For now Sutherland-Cohen algorithm is only used to check the line is actually visible,
    // then test for every point inside the main Bresenham's loop.
    if (!clipLine(X1, Y1, X2, Y2, m_paintState.absClippingRect, true))  // true = do not change line coordinates!
      return;
    const int dx = abs(X2 - X1);
    const int dy = abs(Y2 - Y1);
    const int sx = X1 < X2 ? 1 : -1;
    const int sy = Y1 < Y2 ? 1 : -1;
    int err = (dx > dy ? dx : -dy) / 2;
    while (true) {
      if (m_paintState.absClippingRect.contains(X1, Y1))
        VGA_PIXEL(X1, Y1) = pattern;
      if (X1 == X2 && Y1 == Y2)
        break;
      int e2 = err;
      if (e2 > -dx) {
        err -= dy;
        X1 += sx;
      }
      if (e2 < dy) {
        err += dx;
        Y1 += sy;
      }
    }
  }
}


// parameters not checked
void IRAM_ATTR VGAController::fillRow(int y, int x1, int x2, uint8_t pattern)
{
  uint8_t * row = (uint8_t*) m_viewPort[y];
  // fill first bytes before full 32 bits word
  int x = x1;
  for (; x <= x2 && (x & 3) != 0; ++x) {
    VGA_PIXELINROW(row, x) = pattern;
  }
  // fill whole 32 bits words (don't care about VGA_PIXELINROW adjusted alignment)
  if (x <= x2) {
    int sz = (x2 & ~3) - x;
    memset(row + x, pattern, sz);
    x += sz;
  }
  // fill last unaligned bytes
  for (; x <= x2; ++x) {
    VGA_PIXELINROW(row, x) = pattern;
  }
}


// swaps all pixels inside the range x1...x2 of yA and yB
// parameters not checked
void IRAM_ATTR VGAController::swapRows(int yA, int yB, int x1, int x2)
{
  uint8_t * rowA = (uint8_t*) m_viewPort[yA];
  uint8_t * rowB = (uint8_t*) m_viewPort[yB];
  // swap first bytes before full 32 bits word
  int x = x1;
  for (; x <= x2 && (x & 3) != 0; ++x)
    tswap(VGA_PIXELINROW(rowA, x), VGA_PIXELINROW(rowB, x));
  // swap whole 32 bits words (don't care about VGA_PIXELINROW adjusted alignment)
  uint32_t * a = (uint32_t*)(rowA + x);
  uint32_t * b = (uint32_t*)(rowB + x);
  for (int right = (x2 & ~3); x < right; x += 4)
    tswap(*a++, *b++);
  // swap last unaligned bytes
  for (x = (x2 & ~3); x <= x2; ++x)
    tswap(VGA_PIXELINROW(rowA, x), VGA_PIXELINROW(rowB, x));
}


void IRAM_ATTR VGAController::execDrawRect(Rect const & rect)
{
  int x1 = (rect.X1 < rect.X2 ? rect.X1 : rect.X2) + m_paintState.origin.X;
  int y1 = (rect.Y1 < rect.Y2 ? rect.Y1 : rect.Y2) + m_paintState.origin.Y;
  int x2 = (rect.X1 < rect.X2 ? rect.X2 : rect.X1) + m_paintState.origin.X;
  int y2 = (rect.Y1 < rect.Y2 ? rect.Y2 : rect.Y1) + m_paintState.origin.Y;

  hideSprites();
  uint8_t pattern = m_paintState.paintOptions.swapFGBG ? preparePixel(m_paintState.brushColor) : preparePixel(m_paintState.penColor);

  drawLine(x1 + 1, y1,     x2, y1, pattern);
  drawLine(x2,     y1 + 1, x2, y2, pattern);
  drawLine(x2 - 1, y2,     x1, y2, pattern);
  drawLine(x1,     y2 - 1, x1, y1, pattern);
}


void IRAM_ATTR VGAController::execFillRect(Rect const & rect)
{
  int x1 = (rect.X1 < rect.X2 ? rect.X1 : rect.X2) + m_paintState.origin.X;
  int y1 = (rect.Y1 < rect.Y2 ? rect.Y1 : rect.Y2) + m_paintState.origin.Y;
  int x2 = (rect.X1 < rect.X2 ? rect.X2 : rect.X1) + m_paintState.origin.X;
  int y2 = (rect.Y1 < rect.Y2 ? rect.Y2 : rect.Y1) + m_paintState.origin.Y;

  const int clipX1 = m_paintState.absClippingRect.X1;
  const int clipY1 = m_paintState.absClippingRect.Y1;
  const int clipX2 = m_paintState.absClippingRect.X2;
  const int clipY2 = m_paintState.absClippingRect.Y2;

  if (x1 > clipX2 || x2 < clipX1 || y1 > clipY2 || y2 < clipY1)
    return;

  x1 = iclamp(x1, clipX1, clipX2);
  y1 = iclamp(y1, clipY1, clipY2);
  x2 = iclamp(x2, clipX1, clipX2);
  y2 = iclamp(y2, clipY1, clipY2);

  hideSprites();
  uint8_t pattern = m_paintState.paintOptions.swapFGBG ? preparePixel(m_paintState.penColor) : preparePixel(m_paintState.brushColor);

  for (int y = y1; y <= y2; ++y)
    fillRow(y, x1, x2, pattern);
}


void IRAM_ATTR VGAController::execFillEllipse(Size const & size)
{
  hideSprites();
  uint8_t pattern = m_paintState.paintOptions.swapFGBG ? preparePixel(m_paintState.penColor) : preparePixel(m_paintState.brushColor);

  const int clipX1 = m_paintState.absClippingRect.X1;
  const int clipY1 = m_paintState.absClippingRect.Y1;
  const int clipX2 = m_paintState.absClippingRect.X2;
  const int clipY2 = m_paintState.absClippingRect.Y2;

  const int halfWidth  = size.width / 2;
  const int halfHeight = size.height / 2;
  const int hh = halfHeight * halfHeight;
  const int ww = halfWidth * halfWidth;
  const int hhww = hh * ww;

  int x0 = halfWidth;
  int dx = 0;

  int centerX = m_paintState.position.X;
  int centerY = m_paintState.position.Y;

  if (centerY >= clipY1 && centerY <= clipY2) {
    int col1 = centerX - halfWidth;
    int col2 = centerX + halfWidth;
    if (col1 <= clipX2 && col2 >= clipX1) {
      col1 = iclamp(col1, clipX1, clipX2);
      col2 = iclamp(col2, clipX1, clipX2);
      fillRow(centerY, col1, col2, pattern);
    }
  }

  for (int y = 1; y <= halfHeight; ++y)
  {
    int x1 = x0 - (dx - 1);
    for ( ; x1 > 0; x1--)
      if (x1 * x1 * hh + y * y * ww <= hhww)
        break;
    dx = x0 - x1;
    x0 = x1;

    int col1 = centerX - x0;
    int col2 = centerX + x0;

    if (col1 <= clipX2 && col2 >= clipX1) {

      col1 = iclamp(col1, clipX1, clipX2);
      col2 = iclamp(col2, clipX1, clipX2);

      int y1 = centerY - y;
      if (y1 >= clipY1 && y1 <= clipY2)
        fillRow(y1, col1, col2, pattern);

      int y2 = centerY + y;
      if (y2 >= clipY1 && y2 <= clipY2)
        fillRow(y2, col1, col2, pattern);

    }
  }
}


void IRAM_ATTR VGAController::execDrawEllipse(Size const & size)
{
  hideSprites();
  uint8_t pattern = m_paintState.paintOptions.swapFGBG ? preparePixel(m_paintState.brushColor) : preparePixel(m_paintState.penColor);

  const int clipX1 = m_paintState.absClippingRect.X1;
  const int clipY1 = m_paintState.absClippingRect.Y1;
  const int clipX2 = m_paintState.absClippingRect.X2;
  const int clipY2 = m_paintState.absClippingRect.Y2;

  int x0 = m_paintState.position.X - size.width / 2;
  int y0 = m_paintState.position.Y - size.height / 2;
  int x1 = m_paintState.position.X + size.width / 2;
  int y1 = m_paintState.position.Y + size.height / 2;

  int a = abs (x1 - x0), b = abs (y1 - y0), b1 = b & 1;
  int dx = 4 * (1 - a) * b * b, dy = 4 * (b1 + 1) * a * a;
  int err = dx + dy + b1 * a * a, e2;

  if (x0 > x1) {
    x0 = x1;
    x1 += a;
  }
  if (y0 > y1)
    y0 = y1;
  y0 += (b + 1) / 2;
  y1 = y0-b1;
  a *= 8 * a;
  b1 = 8 * b * b;
  do {
    if (y0 >= clipY1 && y0 <= clipY2) {
      if (x1 >= clipX1 && x1 <= clipX2) {
        // bottom-right semicircle
        VGA_PIXEL(x1, y0) = pattern;
      }
      if (x0 >= clipX1 && x0 <= clipX2) {
        // bottom-left semicircle
        VGA_PIXEL(x0, y0) = pattern;
      }
    }
    if (y1 >= clipY1 && y1 <= clipY2) {
      if (x0 >= clipX1 && x0 <= clipX2) {
        // top-left semicircle
        VGA_PIXEL(x0, y1) = pattern;
      }
      if (x1 >= clipX1 && x1 <= clipX2) {
        // top-right semicircle
        VGA_PIXEL(x1, y1) = pattern;
      }
    }
    e2 = 2 * err;
    if (e2 >= dx) {
      ++x0;
      --x1;
      err += dx += b1;
    }
    if (e2 <= dy) {
      ++y0;
      --y1;
      err += dy += a;
    }
  } while (x0 <= x1);

  while (y0 - y1 < b) {
    int x = x0 - 1;
    int y = y0;
    if (x >= clipX1 && x <= clipX2 && y >= clipY1 && y <= clipY2)
      VGA_PIXEL(x, y) = pattern;

    x = x1 + 1;
    y = y0++;
    if (x >= clipX1 && x <= clipX2 && y >= clipY1 && y <= clipY2)
      VGA_PIXEL(x, y) = pattern;

    x = x0 - 1;
    y = y1;
    if (x >= clipX1 && x <= clipX2 && y >= clipY1 && y <= clipY2)
      VGA_PIXEL(x, y) = pattern;

    x = x1 + 1;
    y = y1--;
    if (x >= clipX1 && x <= clipX2 && y >= clipY1 && y <= clipY2)
      VGA_PIXEL(x, y) = pattern;
  }
}


void IRAM_ATTR VGAController::execClear()
{
  hideSprites();
  uint8_t pattern = m_paintState.paintOptions.swapFGBG ? preparePixel(m_paintState.penColor) : preparePixel(m_paintState.brushColor);
  for (int y = 0; y < m_viewPortHeight; ++y)
    memset((uint8_t*) m_viewPort[y], pattern, m_viewPortWidth);
}


// scroll < 0 -> scroll UP
// scroll > 0 -> scroll DOWN
// Speciying horizontal scrolling region slow-down scrolling!
void IRAM_ATTR VGAController::execVScroll(int scroll)
{
  hideSprites();
  uint8_t pattern = m_paintState.paintOptions.swapFGBG ? preparePixel(m_paintState.penColor) : preparePixel(m_paintState.brushColor);
  int Y1 = m_paintState.scrollingRegion.Y1;
  int Y2 = m_paintState.scrollingRegion.Y2;
  int X1 = m_paintState.scrollingRegion.X1;
  int X2 = m_paintState.scrollingRegion.X2;
  int height = Y2 - Y1 + 1;

  if (scroll < 0) {

    // scroll UP

    for (int i = 0; i < height + scroll; ++i) {

      // these are necessary to maintain invariate out of scrolling regions
      if (X1 > 0)
        swapRows(Y1 + i, Y1 + i - scroll, 0, X1 - 1);
      if (X2 < m_viewPortWidth - 1)
        swapRows(Y1 + i, Y1 + i - scroll, X2 + 1, m_viewPortWidth - 1);

      // swap scan lines
      tswap(m_viewPort[Y1 + i], m_viewPort[Y1 + i - scroll]);
    }

    // fill lower area with brush color
    for (int i = height + scroll; i < height; ++i)
      fillRow(Y1 + i, X1, X2, pattern);

  } else if (scroll > 0) {

    // scroll DOWN
    for (int i = height - scroll - 1; i >= 0; --i) {

      // these are necessary to maintain invariate out of scrolling regions
      if (X1 > 0)
        swapRows(Y1 + i, Y1 + i + scroll, 0, X1 - 1);
      if (X2 < m_viewPortWidth - 1)
        swapRows(Y1 + i, Y1 + i + scroll, X2 + 1, m_viewPortWidth - 1);

      // swap scan lines
      tswap(m_viewPort[Y1 + i], m_viewPort[Y1 + i + scroll]);
    }

    // fill upper area with brush color
    for (int i = 0; i < scroll; ++i)
      fillRow(Y1 + i, X1, X2, pattern);

  }

  if (scroll != 0) {

    // reassign DMA pointers

    int viewPortBuffersPerLine = 0;
    int linePos = 1;
    switch (m_timings.HStartingBlock) {
      case VGAScanStart::FrontPorch:
        // FRONTPORCH -> SYNC -> BACKPORCH -> VISIBLEAREA
        viewPortBuffersPerLine = (m_viewPortCol + m_viewPortWidth) < m_timings.HVisibleArea ? 3 : 2;
        break;
      case VGAScanStart::Sync:
        // SYNC -> BACKPORCH -> VISIBLEAREA -> FRONTPORCH
        viewPortBuffersPerLine = 3;
        break;
      case VGAScanStart::BackPorch:
        // BACKPORCH -> VISIBLEAREA -> FRONTPORCH -> SYNC
        viewPortBuffersPerLine = 3;
        break;
      case VGAScanStart::VisibleArea:
        // VISIBLEAREA -> FRONTPORCH -> SYNC -> BACKPORCH
        viewPortBuffersPerLine = m_viewPortCol > 0 ? 3 : 2;
        linePos = m_viewPortCol > 0 ? 1 : 0;
        break;
    }
    for (int i = Y1, idx = Y1 * m_timings.scanCount; i <= Y2; ++i)
      for (int scan = 0; scan < m_timings.scanCount; ++scan, ++idx)
        setDMABufferView(m_viewPortRow * m_timings.scanCount + idx * viewPortBuffersPerLine + linePos, i, scan, m_viewPort, false);
  }
}


// Scrolling by 1, 2, 3 and 4 pixels is optimized. Also scrolling multiples of 4 (8, 16, 24...) is optimized.
// Scrolling by other values requires up to three steps (scopose scrolling by 1, 2, 3 or 4): for example scrolling by 5 is scomposed to 4 and 1, scrolling
// by 6 is 4 + 2, etc.
// Horizontal scrolling region start and size (X2-X1+1) must be aligned to 32 bits, otherwise the unoptimized (very slow) version is used.
void IRAM_ATTR VGAController::execHScroll(int scroll)
{
  hideSprites();
  uint8_t pattern8   = m_paintState.paintOptions.swapFGBG ? preparePixel(m_paintState.penColor) : preparePixel(m_paintState.brushColor);
  uint16_t pattern16 = pattern8 << 8 | pattern8;
  uint32_t pattern32 = pattern16 << 16 | pattern16;

  int Y1 = m_paintState.scrollingRegion.Y1;
  int Y2 = m_paintState.scrollingRegion.Y2;
  int X1 = m_paintState.scrollingRegion.X1;
  int X2 = m_paintState.scrollingRegion.X2;

  int width   = X2 - X1 + 1;
  int width32 = width >> 2;
  bool HScrolllingRegionAligned = ((X1 & 3) == 0 && (width & 3) == 0);

  if (scroll < 0) {
    // scroll left
    for (int y = Y1; y <= Y2; ++y) {
      if (HScrolllingRegionAligned) {
        // aligned horizontal scrolling region, fast version
        uint8_t * row = (uint8_t*) (m_viewPort[y] + X1);
        for (int s = -scroll; s > 0;) {
          if (s >= 4) {
            // scroll left 4, 8 12, etc.. pixels moving 32 bit words
            uint8_t * w = row;
            int sz = (s & ~3) >> 2;
            for (int i = 0; i < width32 - sz; ++i, w += 4)
              *((uint32_t*)w) = *((uint32_t*)w + sz);
            for (int i = tmax(0, width32 - sz); i < width32; ++i, w += 4)
              *((uint32_t*)w) = pattern32;
            s -= (s & ~3);
          } else if ((s & 3) == 3) {
            // scroll left 3 pixels swapping 8 bit words
            uint8_t * b = row;
            for (int i = 1; i < width32; ++i, b += 4) {
              b[2] = b[1];
              b[1] = b[4];
              b[0] = b[7];
              b[3] = b[6];
            }
            b[2] = b[1];
            b[1] = b[0] = b[3] = pattern8;
            s -= 3;
          } else if (s & 2) {
            // scroll left 2 pixels swapping 16 bit words
            uint16_t * w = (uint16_t*) row;
            for (int i = 1; i < width32; ++i, w += 2) {
              w[1] = w[0];
              w[0] = w[3];
            }
            w[1] = w[0];
            w[0] = pattern16;
            s -= 2;
          } else if (s & 1) {
            // scroll left 1 pixel by rotating 32 bit words
            uint8_t * w = row;
            for (int i = 1; i < width32; ++i, w += 4) {
              *((uint32_t*)w) = *((uint32_t*)w) >> 8 | *((uint32_t*)w) << 24;
              w[1] = w[6];
            }
            *((uint32_t*)w) = *((uint32_t*)w) >> 8 | *((uint32_t*)w) << 24;
            w[1] = pattern8;
            --s;
          }
        }
      } else {
        // unaligned horizontal scrolling region, fallback to slow version
        uint8_t * row = (uint8_t*) m_viewPort[y];
        for (int x = X1; x <= X2 + scroll; ++x)
          VGA_PIXELINROW(row, x) = VGA_PIXELINROW(row, x - scroll);
        // fill right area with brush color
        for (int x = X2 + 1 + scroll; x <= X2; ++x)
          VGA_PIXELINROW(row, x) = pattern8;
      }
    }
  } else if (scroll > 0) {
    // scroll right
    for (int y = Y1; y <= Y2; ++y) {
      if (HScrolllingRegionAligned) {
        // aligned horizontal scrolling region, fast version
        uint8_t * row = (uint8_t*) (m_viewPort[y] + X1);
        for (int s = scroll; s > 0;) {
          if (s >= 4) {
            // scroll right 4, 8 12, etc.. pixels moving 32 bit words
            int sz = (s & ~3) >> 2;
            uint8_t * w = row + width - 4;
            for (int i = 0; i < width32 - sz; ++i, w -= 4)
              *((uint32_t*)w) = *((uint32_t*)w - sz);
            for (int i = tmax(0, width32 - sz); i < width32; ++i, w -= 4)
              *((uint32_t*)w) = pattern32;
            s -= (s & ~3);
          } else if ((s & 3) == 3) {
            // scroll right 3 pixels swapping 8 bit words
            uint8_t * b = row + width - 4;
            for (int i = 1; i < width32; ++i, b -= 4) {
              b[0] = b[-3];
              b[1] = b[2];
              b[2] = b[-1];
              b[3] = b[-4];
            }
            b[1] = b[2];
            b[0] = b[2] = b[3] = pattern8;
            s -= 3;
          } else if (s & 2) {
            // scroll right 2 pixels swapping 16 bit words
            uint16_t * w = (uint16_t*) (row + width - 4);
            for (int i = 1; i < width32; ++i, w -= 2) {
              w[0] = w[1];
              w[1] = w[-2];
            }
            w[0] = w[1];
            w[1] = pattern16;
            s -= 2;
          } else if (s & 1) {
            // scroll right 1 pixel by rotating 32 bit words
            uint8_t * w = row + width - 4;
            for (int i = 1; i < width32; ++i, w -= 4) {
              *((uint32_t*)w) = *((uint32_t*)w) << 8 | *((uint32_t*)w) >> 24;
              w[2] = w[-3];
            }
            *((uint32_t*)w) = *((uint32_t*)w) << 8 | *((uint32_t*)w) >> 24;
            w[2] = pattern8;
            --s;
          }
        }
      } else {
        // unaligned horizontal scrolling region, fallback to slow version
        uint8_t * row = (uint8_t*) m_viewPort[y];
        for (int x = X2 - scroll; x >= X1; --x)
          VGA_PIXELINROW(row, x + scroll) = VGA_PIXELINROW(row, x);
        // fill left area with brush color
        for (int x = X1; x < X1 + scroll; ++x)
          VGA_PIXELINROW(row, x) = pattern8;
      }
    }

  }
}


void IRAM_ATTR VGAController::execRenderGlyphsBuffer(GlyphsBufferRenderInfo const & glyphsBufferRenderInfo)
{
  hideSprites();
  int itemX = glyphsBufferRenderInfo.itemX;
  int itemY = glyphsBufferRenderInfo.itemY;

  int glyphsWidth  = glyphsBufferRenderInfo.glyphsBuffer->glyphsWidth;
  int glyphsHeight = glyphsBufferRenderInfo.glyphsBuffer->glyphsHeight;

  uint32_t const * mapItem = glyphsBufferRenderInfo.glyphsBuffer->map + itemX + itemY * glyphsBufferRenderInfo.glyphsBuffer->columns;

  GlyphOptions glyphOptions = glyphMapItem_getOptions(mapItem);
  auto fgColor = RGB222(glyphMapItem_getFGColor(mapItem));
  auto bgColor = RGB222(glyphMapItem_getBGColor(mapItem));

  Glyph glyph;
  glyph.X      = (int16_t) (itemX * glyphsWidth * (glyphOptions.doubleWidth ? 2 : 1));
  glyph.Y      = (int16_t) (itemY * glyphsHeight);
  glyph.width  = glyphsWidth;
  glyph.height = glyphsHeight;
  glyph.data   = glyphsBufferRenderInfo.glyphsBuffer->glyphsData + glyphMapItem_getIndex(mapItem) * glyphsHeight * ((glyphsWidth + 7) / 8);;

  execDrawGlyph(glyph, glyphOptions, fgColor, bgColor);
}


void IRAM_ATTR VGAController::execDrawGlyph(Glyph const & glyph, GlyphOptions glyphOptions, RGB222 penColor, RGB222 brushColor)
{
  hideSprites();
  if (!glyphOptions.bold && !glyphOptions.italic && !glyphOptions.blank && !glyphOptions.underline && !glyphOptions.doubleWidth && glyph.width <= 32)
    execDrawGlyph_light(glyph, glyphOptions, penColor, brushColor);
  else
    execDrawGlyph_full(glyph, glyphOptions, penColor, brushColor);
}


// TODO: Italic doesn't work well when clipping rect is specified
void IRAM_ATTR VGAController::execDrawGlyph_full(Glyph const & glyph, GlyphOptions glyphOptions, RGB222 penColor, RGB222 brushColor)
{
  const int clipX1 = m_paintState.absClippingRect.X1;
  const int clipY1 = m_paintState.absClippingRect.Y1;
  const int clipX2 = m_paintState.absClippingRect.X2;
  const int clipY2 = m_paintState.absClippingRect.Y2;

  const int origX = m_paintState.origin.X;
  const int origY = m_paintState.origin.Y;

  const int glyphX = glyph.X + origX;
  const int glyphY = glyph.Y + origY;

  if (glyphX > clipX2 || glyphY > clipY2)
    return;

  int16_t glyphWidth        = glyph.width;
  int16_t glyphHeight       = glyph.height;
  uint8_t const * glyphData = glyph.data;
  int16_t glyphWidthByte    = (glyphWidth + 7) / 8;
  int16_t glyphSize         = glyphHeight * glyphWidthByte;

  bool fillBackground = glyphOptions.fillBackground;
  bool bold           = glyphOptions.bold;
  bool italic         = glyphOptions.italic;
  bool blank          = glyphOptions.blank;
  bool underline      = glyphOptions.underline;
  int doubleWidth     = glyphOptions.doubleWidth;

  // modify glyph to handle top half and bottom half double height
  // doubleWidth = 1 is handled directly inside drawing routine
  if (doubleWidth > 1) {
    uint8_t * newGlyphData = (uint8_t*) alloca(glyphSize);
    // doubling top-half or doubling bottom-half?
    int offset = (doubleWidth == 2 ? 0 : (glyphHeight >> 1));
    for (int y = 0; y < glyphHeight ; ++y)
      for (int x = 0; x < glyphWidthByte; ++x)
        newGlyphData[x + y * glyphWidthByte] = glyphData[x + (offset + (y >> 1)) * glyphWidthByte];
    glyphData = newGlyphData;
  }

  // a very simple and ugly skew (italic) implementation!
  int skewAdder = 0, skewH1 = 0, skewH2 = 0;
  if (italic) {
    skewAdder = 2;
    skewH1 = glyphHeight / 3;
    skewH2 = skewH1 * 2;
  }

  int16_t X1 = 0;
  int16_t XCount = glyphWidth;
  int16_t destX = glyphX;

  if (destX < clipX1) {
    X1 = (clipX1 - destX) / (doubleWidth ? 2 : 1);
    destX = clipX1;
  }
  if (X1 >= glyphWidth)
    return;

  if (destX + XCount + skewAdder > clipX2 + 1)
    XCount = clipX2 + 1 - destX - skewAdder;
  if (X1 + XCount > glyphWidth)
    XCount = glyphWidth - X1;

  int16_t Y1 = 0;
  int16_t YCount = glyphHeight;
  int destY = glyphY;

  if (destY < clipY1) {
    Y1 = clipY1 - destY;
    destY = clipY1;
  }
  if (Y1 >= glyphHeight)
    return;

  if (destY + YCount > clipY2 + 1)
    YCount = clipY2 + 1 - destY;
  if (Y1 + YCount > glyphHeight)
    YCount = glyphHeight - Y1;

  if (glyphOptions.invert ^ m_paintState.paintOptions.swapFGBG)
    tswap(penColor, brushColor);

  // a very simple and ugly reduce luminosity (faint) implementation!
  if (glyphOptions.reduceLuminosity) {
    if (penColor.R > 2) penColor.R -= 1;
    if (penColor.G > 2) penColor.G -= 1;
    if (penColor.B > 2) penColor.B -= 1;
  }

  uint8_t penPattern   = preparePixel(penColor);
  uint8_t brushPattern = preparePixel(brushColor);

  for (int y = Y1; y < Y1 + YCount; ++y, ++destY) {

    // true if previous pixel has been set
    bool prevSet = false;

    uint8_t * dstrow = (uint8_t*) m_viewPort[destY];
    uint8_t const * srcrow = glyphData + y * glyphWidthByte;

    if (underline && y == glyphHeight - FABGLIB_UNDERLINE_POSITION - 1) {

      for (int x = X1, adestX = destX + skewAdder; x < X1 + XCount && adestX <= clipX2; ++x, ++adestX) {
        VGA_PIXELINROW(dstrow, adestX) = blank ? brushPattern : penPattern;
        if (doubleWidth) {
          ++adestX;
          if (adestX > clipX2)
            break;
          VGA_PIXELINROW(dstrow, adestX) = blank ? brushPattern : penPattern;
        }
      }

    } else {

      for (int x = X1, adestX = destX + skewAdder; x < X1 + XCount && adestX <= clipX2; ++x, ++adestX) {
        if ((srcrow[x >> 3] << (x & 7)) & 0x80 && !blank) {
          VGA_PIXELINROW(dstrow, adestX) = penPattern;
          prevSet = true;
        } else if (bold && prevSet) {
          VGA_PIXELINROW(dstrow, adestX) = penPattern;
          prevSet = false;
        } else if (fillBackground) {
          VGA_PIXELINROW(dstrow, adestX) = brushPattern;
          prevSet = false;
        } else {
          prevSet = false;
        }
        if (doubleWidth) {
          ++adestX;
          if (adestX > clipX2)
            break;
          if (fillBackground)
            VGA_PIXELINROW(dstrow, adestX) = prevSet ? penPattern : brushPattern;
          else if (prevSet)
            VGA_PIXELINROW(dstrow, adestX) = penPattern;
        }
      }

    }

    if (italic && (y == skewH1 || y == skewH2))
      --skewAdder;

  }
}


// assume:
//   glyph.width <= 32
//   glyphOptions.fillBackground = 0 or 1
//   glyphOptions.invert : 0 or 1
//   glyphOptions.reduceLuminosity: 0 or 1
//   glyphOptions.... others = 0
//   m_paintState.paintOptions.swapFGBG: 0 or 1
void IRAM_ATTR VGAController::execDrawGlyph_light(Glyph const & glyph, GlyphOptions glyphOptions, RGB222 penColor, RGB222 brushColor)
{
  const int clipX1 = m_paintState.absClippingRect.X1;
  const int clipY1 = m_paintState.absClippingRect.Y1;
  const int clipX2 = m_paintState.absClippingRect.X2;
  const int clipY2 = m_paintState.absClippingRect.Y2;

  const int origX = m_paintState.origin.X;
  const int origY = m_paintState.origin.Y;

  const int glyphX = glyph.X + origX;
  const int glyphY = glyph.Y + origY;

  if (glyphX > clipX2 || glyphY > clipY2)
    return;

  int16_t glyphWidth        = glyph.width;
  int16_t glyphHeight       = glyph.height;
  uint8_t const * glyphData = glyph.data;
  int16_t glyphWidthByte    = (glyphWidth + 7) / 8;

  int16_t X1 = 0;
  int16_t XCount = glyphWidth;
  int16_t destX = glyphX;

  int16_t Y1 = 0;
  int16_t YCount = glyphHeight;
  int destY = glyphY;

  if (destX < clipX1) {
    X1 = clipX1 - destX;
    destX = clipX1;
  }
  if (X1 >= glyphWidth)
    return;

  if (destX + XCount > clipX2 + 1)
    XCount = clipX2 + 1 - destX;
  if (X1 + XCount > glyphWidth)
    XCount = glyphWidth - X1;

  if (destY < clipY1) {
    Y1 = clipY1 - destY;
    destY = clipY1;
  }
  if (Y1 >= glyphHeight)
    return;

  if (destY + YCount > clipY2 + 1)
    YCount = clipY2 + 1 - destY;
  if (Y1 + YCount > glyphHeight)
    YCount = glyphHeight - Y1;

  if (glyphOptions.invert ^ m_paintState.paintOptions.swapFGBG)
    tswap(penColor, brushColor);

  // a very simple and ugly reduce luminosity (faint) implementation!
  if (glyphOptions.reduceLuminosity) {
    if (penColor.R > 2) penColor.R -= 1;
    if (penColor.G > 2) penColor.G -= 1;
    if (penColor.B > 2) penColor.B -= 1;
  }

  bool fillBackground = glyphOptions.fillBackground;

  uint8_t penPattern   = preparePixel(penColor);
  uint8_t brushPattern = preparePixel(brushColor);

  for (int y = Y1; y < Y1 + YCount; ++y, ++destY) {
    uint8_t * dstrow = (uint8_t*) m_viewPort[destY];
    uint8_t const * srcrow = glyphData + y * glyphWidthByte;

    uint32_t src = (srcrow[0] << 24) | (srcrow[1] << 16) | (srcrow[2] << 8) | (srcrow[3]);
    src <<= X1;
    if (fillBackground) {
      // filled background
      for (int x = X1, adestX = destX; x < X1 + XCount; ++x, ++adestX, src <<= 1)
        VGA_PIXELINROW(dstrow, adestX) = src & 0x80000000 ? penPattern : brushPattern;
    } else {
      // transparent background
      for (int x = X1, adestX = destX; x < X1 + XCount; ++x, ++adestX, src <<= 1)
        if (src & 0x80000000)
          VGA_PIXELINROW(dstrow, adestX) = penPattern;
    }
  }
}


void IRAM_ATTR VGAController::execInvertRect(Rect const & rect)
{
  hideSprites();

  const uint8_t HVSync = packHVSync();

  const int origX = m_paintState.origin.X;
  const int origY = m_paintState.origin.Y;

  const int clipX1 = m_paintState.absClippingRect.X1;
  const int clipY1 = m_paintState.absClippingRect.Y1;
  const int clipX2 = m_paintState.absClippingRect.X2;
  const int clipY2 = m_paintState.absClippingRect.Y2;

  const int x1 = iclamp(rect.X1 + origX, clipX1, clipX2);
  const int y1 = iclamp(rect.Y1 + origY, clipY1, clipY2);
  const int x2 = iclamp(rect.X2 + origX, clipX1, clipX2);
  const int y2 = iclamp(rect.Y2 + origY, clipY1, clipY2);

  for (int y = y1; y <= y2; ++y) {
    uint8_t * row = (uint8_t*) m_viewPort[y];
    for (int x = x1; x <= x2; ++x) {
      uint8_t * px = (uint8_t*) &VGA_PIXELINROW(row, x);
      *px = HVSync | ~(*px);
    }
  }
}


void IRAM_ATTR VGAController::execSwapFGBG(Rect const & rect)
{
  hideSprites();

  uint8_t penPattern   = preparePixel(m_paintState.penColor);
  uint8_t brushPattern = preparePixel(m_paintState.brushColor);

  int origX = m_paintState.origin.X;
  int origY = m_paintState.origin.Y;

  int x1 = iclamp(rect.X1 + origX, 0, m_viewPortWidth - 1);
  int y1 = iclamp(rect.Y1 + origY, 0, m_viewPortHeight - 1);
  int x2 = iclamp(rect.X2 + origX, 0, m_viewPortWidth - 1);
  int y2 = iclamp(rect.Y2 + origY, 0, m_viewPortHeight - 1);
  for (int y = y1; y <= y2; ++y) {
    uint8_t * row = (uint8_t*) m_viewPort[y];
    for (int x = x1; x <= x2; ++x) {
      uint8_t * px = (uint8_t*) &VGA_PIXELINROW(row, x);
      if (*px == penPattern)
        *px = brushPattern;
      else if (*px == brushPattern)
        *px = penPattern;
    }
  }
}


// Slow operation!
// supports overlapping of source and dest rectangles
void IRAM_ATTR VGAController::execCopyRect(Rect const & source)
{
  hideSprites();

  const int clipX1 = m_paintState.absClippingRect.X1;
  const int clipY1 = m_paintState.absClippingRect.Y1;
  const int clipX2 = m_paintState.absClippingRect.X2;
  const int clipY2 = m_paintState.absClippingRect.Y2;

  int origX = m_paintState.origin.X;
  int origY = m_paintState.origin.Y;

  int srcX = source.X1 + origX;
  int srcY = source.Y1 + origY;
  int width  = source.X2 - source.X1 + 1;
  int height = source.Y2 - source.Y1 + 1;
  int destX = m_paintState.position.X;
  int destY = m_paintState.position.Y;
  int deltaX = destX - srcX;
  int deltaY = destY - srcY;

  int incX = deltaX < 0 ? 1 : -1;
  int incY = deltaY < 0 ? 1 : -1;

  int startX = deltaX < 0 ? destX : destX + width - 1;
  int startY = deltaY < 0 ? destY : destY + height - 1;

  for (int y = startY, i = 0; i < height; y += incY, ++i) {
    if (y >= clipY1 && y <= clipY2) {
      uint8_t * srcRow = (uint8_t*) m_viewPort[y - deltaY];
      uint8_t * dstRow = (uint8_t*) m_viewPort[y];
      for (int x = startX, j = 0; j < width; x += incX, ++j) {
        if (x >= clipX1 && x <= clipX2)
          VGA_PIXELINROW(dstRow, x) = VGA_PIXELINROW(srcRow, x - deltaX);
      }
    }
  }
}


// no bounds check is done!
void VGAController::readScreen(Rect const & rect, RGB888 * destBuf)
{
  for (int y = rect.Y1; y <= rect.Y2; ++y) {
    uint8_t * row = (uint8_t*) m_viewPort[y];
    for (int x = rect.X1; x <= rect.X2; ++x, ++destBuf) {
      uint8_t rawpix = VGA_PIXELINROW(row, x);
      *destBuf = RGB888((rawpix & 3) * 85, ((rawpix >> 2) & 3) * 85, ((rawpix >> 4) & 3) * 85);
    }
  }
}


// no bounds check is done!
void VGAController::readScreen(Rect const & rect, RGB222 * destBuf)
{
  uint8_t * dbuf = (uint8_t*) destBuf;
  for (int y = rect.Y1; y <= rect.Y2; ++y) {
    uint8_t * row = (uint8_t*) m_viewPort[y];
    for (int x = rect.X1; x <= rect.X2; ++x, ++dbuf)
      *dbuf = VGA_PIXELINROW(row, x) & ~VGA_SYNC_MASK;
  }
}


// no bounds check is done!
void VGAController::writeScreen(Rect const & rect, RGB222 * srcBuf)
{
  uint8_t * sbuf = (uint8_t*) srcBuf;
  const uint8_t HVSync = packHVSync();
  for (int y = rect.Y1; y <= rect.Y2; ++y) {
    uint8_t * row = (uint8_t*) m_viewPort[y];
    for (int x = rect.X1; x <= rect.X2; ++x, ++sbuf)
      VGA_PIXELINROW(row, x) = *sbuf | HVSync;
  }
}


void IRAM_ATTR VGAController::execDrawBitmap(BitmapDrawingInfo const & bitmapDrawingInfo)
{
  hideSprites();
  drawBitmap(bitmapDrawingInfo.X + m_paintState.origin.X, bitmapDrawingInfo.Y + m_paintState.origin.Y, bitmapDrawingInfo.bitmap, nullptr, false);
}


void IRAM_ATTR VGAController::drawBitmap_Mask(int destX, int destY, Bitmap const * bitmap, uint8_t * saveBackground, int X1, int Y1, int XCount, int YCount)
{
  const int     width  = bitmap->width;
  const uint8_t HVSync = packHVSync();

  if (saveBackground) {

    // save background and draw the bitmap
    uint8_t foregroundColor = RGB222(bitmap->foregroundColor).pack();
    uint8_t const * data = bitmap->data;
    int rowlen = (bitmap->width + 7) / 8;
    for (int y = Y1, adestY = destY; y < Y1 + YCount; ++y, ++adestY) {
      uint8_t * dstrow = (uint8_t*) m_viewPort[adestY];
      uint8_t * savePx = saveBackground + y * width + X1;
      uint8_t const * src = data + y * rowlen;
      for (int x = X1, adestX = destX; x < X1 + XCount; ++x, ++adestX, ++savePx) {
        if ((src[x >> 3] << (x & 7)) & 0x80) {
          uint8_t * dstPx = &VGA_PIXELINROW(dstrow, adestX);
          *savePx = *dstPx;
          *dstPx = HVSync | foregroundColor;
        } else {
          *savePx = 0;
        }
      }
    }

  } else {

    // just draw the bitmap
    uint8_t foregroundColor = RGB222(bitmap->foregroundColor).pack();
    uint8_t const * data = bitmap->data;
    int rowlen = (bitmap->width + 7) / 8;
    for (int y = Y1, adestY = destY; y < Y1 + YCount; ++y, ++adestY) {
      uint8_t * dstrow = (uint8_t*) m_viewPort[adestY];
      uint8_t const * src = data + y * rowlen;
      for (int x = X1, adestX = destX; x < X1 + XCount; ++x, ++adestX) {
        if ((src[x >> 3] << (x & 7)) & 0x80)
          VGA_PIXELINROW(dstrow, adestX) = HVSync | foregroundColor;
      }
    }

  }
}


void IRAM_ATTR VGAController::drawBitmap_RGBA2222(int destX, int destY, Bitmap const * bitmap, uint8_t * saveBackground, int X1, int Y1, int XCount, int YCount)
{
  const int     width  = bitmap->width;
  const uint8_t HVSync = packHVSync();

  if (saveBackground) {

    // save background and draw the bitmap
    uint8_t const * data = bitmap->data;
    for (int y = Y1, adestY = destY; y < Y1 + YCount; ++y, ++adestY) {
      uint8_t * dstrow = (uint8_t*) m_viewPort[adestY];
      uint8_t * savePx = saveBackground + y * width + X1;
      uint8_t const * src = data + y * width + X1;
      for (int x = X1, adestX = destX; x < X1 + XCount; ++x, ++adestX, ++savePx, ++src) {
        int alpha = *src >> 6;  // TODO?, alpha blending
        if (alpha) {
          uint8_t * dstPx = &VGA_PIXELINROW(dstrow, adestX);
          *savePx = *dstPx;
          *dstPx = HVSync | *src;
        } else {
          *savePx = 0;
        }
      }
    }

  } else {

    // just draw the bitmap
    uint8_t const * data = bitmap->data;
    for (int y = Y1, adestY = destY; y < Y1 + YCount; ++y, ++adestY) {
      uint8_t * dstrow = (uint8_t*) m_viewPort[adestY];
      uint8_t const * src = data + y * width + X1;
      for (int x = X1, adestX = destX; x < X1 + XCount; ++x, ++adestX, ++src) {
        int alpha = *src >> 6;  // TODO?, alpha blending
        if (alpha)
          VGA_PIXELINROW(dstrow, adestX) = HVSync | *src;
      }
    }

  }
}


void IRAM_ATTR VGAController::drawBitmap_RGBA8888(int destX, int destY, Bitmap const * bitmap, uint8_t * saveBackground, int X1, int Y1, int XCount, int YCount)
{
  const int     width  = bitmap->width;
  const uint8_t HVSync = packHVSync();

  if (saveBackground) {

    // save background and draw the bitmap
    RGBA8888 const * data = (RGBA8888 const *) bitmap->data;
    for (int y = Y1, adestY = destY; y < Y1 + YCount; ++y, ++adestY) {
      uint8_t * dstrow = (uint8_t*) m_viewPort[adestY];
      uint8_t * savePx = saveBackground + y * width + X1;
      RGBA8888 const * src = data + y * width + X1;
      for (int x = X1, adestX = destX; x < X1 + XCount; ++x, ++adestX, ++savePx, ++src) {
        if (src->A) {
          uint8_t * dstPx = &VGA_PIXELINROW(dstrow, adestX);
          *savePx = *dstPx;
          *dstPx = HVSync | (src->R >> 6) | (src->G >> 6 << 2) | (src->B >> 6 << 4);
        } else {
          *savePx = 0;
        }
      }
    }

  } else {

    // just draw the bitmap
    RGBA8888 const * data = (RGBA8888 const *) bitmap->data;
    for (int y = Y1, adestY = destY; y < Y1 + YCount; ++y, ++adestY) {
      uint8_t * dstrow = (uint8_t*) m_viewPort[adestY];
      RGBA8888 const * src = data + y * width + X1;
      for (int x = X1, adestX = destX; x < X1 + XCount; ++x, ++adestX, ++src) {
        if (src->A)
          VGA_PIXELINROW(dstrow, adestX) = HVSync | (src->R >> 6) | (src->G >> 6 << 2) | (src->B >> 6 << 4);
      }
    }

  }
}


void IRAM_ATTR VGAController::drawBitmap(int destX, int destY, Bitmap const * bitmap, uint8_t * saveBackground, bool ignoreClippingRect)
{
  const int clipX1 = ignoreClippingRect ? 0 : m_paintState.absClippingRect.X1;
  const int clipY1 = ignoreClippingRect ? 0 : m_paintState.absClippingRect.Y1;
  const int clipX2 = ignoreClippingRect ? m_viewPortWidth - 1 : m_paintState.absClippingRect.X2;
  const int clipY2 = ignoreClippingRect ? m_viewPortHeight - 1 : m_paintState.absClippingRect.Y2;

  if (destX > clipX2 || destY > clipY2)
    return;

  int width  = bitmap->width;
  int height = bitmap->height;

  int X1 = 0;
  int XCount = width;

  if (destX < clipX1) {
    X1 = clipX1 - destX;
    destX = clipX1;
  }
  if (X1 >= width)
    return;

  if (destX + XCount > clipX2 + 1)
    XCount = clipX2 + 1 - destX;
  if (X1 + XCount > width)
    XCount = width - X1;

  int Y1 = 0;
  int YCount = height;

  if (destY < clipY1) {
    Y1 = clipY1 - destY;
    destY = clipY1;
  }
  if (Y1 >= height)
    return;

  if (destY + YCount > clipY2 + 1)
    YCount = clipY2 + 1 - destY;
  if (Y1 + YCount > height)
    YCount = height - Y1;


  switch (bitmap->format) {

    case PixelFormat::Undefined:
      break;

    case PixelFormat::Mask:
      drawBitmap_Mask(destX, destY, bitmap, saveBackground, X1, Y1, XCount, YCount);
      break;

    case PixelFormat::RGBA2222:
      drawBitmap_RGBA2222(destX, destY, bitmap, saveBackground, X1, Y1, XCount, YCount);
      break;

    case PixelFormat::RGBA8888:
      drawBitmap_RGBA8888(destX, destY, bitmap, saveBackground, X1, Y1, XCount, YCount);
      break;

  }

}


void IRAM_ATTR VGAController::execSwapBuffers()
{
  tswap(m_DMABuffers, m_DMABuffersVisible);
  tswap(m_viewPort, m_viewPortVisible);
  m_DMABuffersHead->qe.stqe_next = (lldesc_t*) &m_DMABuffersVisible[0];
}


void IRAM_ATTR VGAController::execDrawPath(Path const & path)
{
  hideSprites();

  uint8_t pattern = m_paintState.paintOptions.swapFGBG ? preparePixel(m_paintState.brushColor) : preparePixel(m_paintState.penColor);

  int origX = m_paintState.origin.X;
  int origY = m_paintState.origin.Y;

  int i = 0;
  for (; i < path.pointsCount - 1; ++i)
    drawLine(path.points[i].X + origX, path.points[i].Y + origY, path.points[i + 1].X + origX, path.points[i + 1].Y + origY, pattern);
  drawLine(path.points[i].X + origX, path.points[i].Y + origY, path.points[0].X + origX, path.points[0].Y + origY, pattern);
}


void IRAM_ATTR VGAController::execFillPath(Path const & path)
{
  hideSprites();

  uint8_t pattern = m_paintState.paintOptions.swapFGBG ? preparePixel(m_paintState.penColor) : preparePixel(m_paintState.brushColor);

  const int clipX1 = m_paintState.absClippingRect.X1;
  const int clipY1 = m_paintState.absClippingRect.Y1;
  const int clipX2 = m_paintState.absClippingRect.X2;
  const int clipY2 = m_paintState.absClippingRect.Y2;

  const int origX = m_paintState.origin.X;
  const int origY = m_paintState.origin.Y;

  int minX = clipX1;
  int maxX = clipX2 + 1;

  int minY = INT_MAX;
  int maxY = 0;
  for (int i = 0; i < path.pointsCount; ++i) {
    int py = path.points[i].Y + origY;
    if (py < minY)
      minY = py;
    if (py > maxY)
      maxY = py;
  }
  minY = tmax(clipY1, minY);
  maxY = tmin(clipY2, maxY);

  int16_t nodeX[path.pointsCount];

  for (int pixelY = minY; pixelY <= maxY; ++pixelY) {

    int nodes = 0;
    int j = path.pointsCount - 1;
    for (int i = 0; i < path.pointsCount; ++i) {
      int piy = path.points[i].Y + origY;
      int pjy = path.points[j].Y + origY;
      if ((piy < pixelY && pjy >= pixelY) || (pjy < pixelY && piy >= pixelY)) {
        int pjx = path.points[j].X + origX;
        int pix = path.points[i].X + origX;
        int a = (pixelY - piy) * (pjx - pix);
        int b = (pjy - piy);
        nodeX[nodes++] = pix + a / b + (((a < 0) ^ (b > 0)) && (a % b));
      }
      j = i;
    }

    int i = 0;
    while (i < nodes - 1) {
      if (nodeX[i] > nodeX[i + 1]) {
        tswap(nodeX[i], nodeX[i + 1]);
        if (i)
          --i;
      } else
        ++i;
    }

    for (int i = 0; i < nodes; i += 2) {
      if (nodeX[i] >= maxX)
        break;
      if (nodeX[i + 1] > minX) {
        if (nodeX[i] < minX)
          nodeX[i] = minX;
        if (nodeX[i + 1] > maxX)
          nodeX[i + 1] = maxX;
        fillRow(pixelY, nodeX[i], nodeX[i + 1] - 1, pattern);
      }
    }
  }
}







} // end of namespace
