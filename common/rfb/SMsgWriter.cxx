/* Copyright (C) 2002-2005 RealVNC Ltd.  All Rights Reserved.
 * Copyright (C) 2011 D. R. Commander.  All Rights Reserved.
 * Copyright 2009-2017 Pierre Ossman for Cendio AB
 * 
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 */
#include <stdio.h>
#include <string>
#include <rdr/OutStream.h>
#include <rdr/MemOutStream.h>
#include <rdr/ZlibOutStream.h>

#include <rfb/msgTypes.h>
#include <rfb/fenceTypes.h>
#include <rfb/clipboardTypes.h>
#include <rfb/Exception.h>
#include <rfb/ConnParams.h>
#include <rfb/UpdateTracker.h>
#include <rfb/Encoder.h>
#include <rfb/SMsgWriter.h>
#include <rfb/LogWriter.h>
#include <rfb/ledStates.h>

using namespace rfb;

static LogWriter vlog("SMsgWriter");

SMsgWriter::SMsgWriter(ConnParams* cp_, rdr::OutStream* os_, rdr::OutStream* udps_)
  : cp(cp_), os(os_), udps(udps_),
    nRectsInUpdate(0), dataRectsInUpdate(0), nRectsInHeader(0),
    needSetDesktopSize(false), needExtendedDesktopSize(false),
    needSetDesktopName(false), needSetCursor(false),
    needSetXCursor(false), needSetCursorWithAlpha(false),
    needSetVMWareCursor(false),
    needCursorPos(false),
    needLEDState(false), needQEMUKeyEvent(false)
{
}

SMsgWriter::~SMsgWriter()
{
}

void SMsgWriter::writeServerInit()
{
  os->writeU16(cp->width);
  os->writeU16(cp->height);
  cp->pf().write(os);
  os->writeString(cp->name());
  endMsg();
}

void SMsgWriter::writeSetColourMapEntries(int firstColour, int nColours,
                                          const rdr::U16 red[],
                                          const rdr::U16 green[],
                                          const rdr::U16 blue[])
{
  startMsg(msgTypeSetColourMapEntries);
  os->pad(1);
  os->writeU16(firstColour);
  os->writeU16(nColours);
  for (int i = firstColour; i < firstColour+nColours; i++) {
    os->writeU16(red[i]);
    os->writeU16(green[i]);
    os->writeU16(blue[i]);
  }
  endMsg();
}

void SMsgWriter::writeBell()
{
  startMsg(msgTypeBell);
  endMsg();
}

void SMsgWriter::writeBinaryClipboard(const std::vector<SConnection::binaryClipboard_t> &b)
{
  startMsg(msgTypeBinaryClipboard);

  os->writeU8(b.size());
  rdr::U8 i;
  for (i = 0; i < b.size(); i++) {
    const rdr::U32 id = b[i].id;
    os->writeU32(id);

    const rdr::U8 mimelen = strlen(b[i].mime);
    os->writeU8(mimelen);
    os->writeBytes(b[i].mime, mimelen);

    os->writeU32(b[i].data.size());
    os->writeBytes(&b[i].data[0], b[i].data.size());
  }

  endMsg();
}

void SMsgWriter::writeStats(const char* str, int len)
{
  startMsg(msgTypeStats);
  os->pad(3);
  os->writeU32(len);
  os->writeBytes(str, len);
  endMsg();
}

void SMsgWriter::writeRequestFrameStats()
{
  startMsg(msgTypeRequestFrameStats);
  endMsg();
}

void SMsgWriter::writeFence(rdr::U32 flags, unsigned len, const char data[])
{
  if (!cp->supportsFence)
    throw Exception("Client does not support fences");
  if (len > 64)
    throw Exception("Too large fence payload");
  if ((flags & ~fenceFlagsSupported) != 0)
    throw Exception("Unknown fence flags");

  startMsg(msgTypeServerFence);
  os->pad(3);

  os->writeU32(flags);

  os->writeU8(len);

  if (len > 0)
    os->writeBytes(data, len);

  endMsg();
}

void SMsgWriter::writeEndOfContinuousUpdates()
{
  if (!cp->supportsContinuousUpdates)
    throw Exception("Client does not support continuous updates");

  startMsg(msgTypeEndOfContinuousUpdates);
  endMsg();
}

bool SMsgWriter::writeSetDesktopSize() {
  if (!cp->supportsDesktopResize)
    return false;

  needSetDesktopSize = true;

  return true;
}

bool SMsgWriter::writeExtendedDesktopSize() {
  if (!cp->supportsExtendedDesktopSize)
    return false;

  needExtendedDesktopSize = true;

  return true;
}

bool SMsgWriter::writeExtendedDesktopSize(rdr::U16 reason, rdr::U16 result,
                                          int fb_width, int fb_height,
                                          const ScreenSet& layout) {
  ExtendedDesktopSizeMsg msg;

  if (!cp->supportsExtendedDesktopSize)
    return false;

  msg.reason = reason;
  msg.result = result;
  msg.fb_width = fb_width;
  msg.fb_height = fb_height;
  msg.layout = layout;

  extendedDesktopSizeMsgs.push_back(msg);

  return true;
}

bool SMsgWriter::writeSetDesktopName() {
  if (!cp->supportsDesktopRename)
    return false;

  needSetDesktopName = true;

  return true;
}

bool SMsgWriter::writeSetCursor()
{
  if (!cp->supportsLocalCursor)
    return false;

  needSetCursor = true;

  return true;
}

bool SMsgWriter::writeSetXCursor()
{
  if (!cp->supportsLocalXCursor)
    return false;

  needSetXCursor = true;

  return true;
}

bool SMsgWriter::writeSetCursorWithAlpha()
{
  if (!cp->supportsLocalCursorWithAlpha)
    return false;

  needSetCursorWithAlpha = true;

  return true;
}

bool SMsgWriter::writeSetVMwareCursor()
{
  if (!cp->supportsVMWareCursor)
    return false;

  needSetVMWareCursor = true;

  return true;
}

void SMsgWriter::writeCursorPos()
{
  if (!cp->supportsEncoding(pseudoEncodingVMwareCursorPosition))
    throw Exception("Client does not support cursor position");

  needCursorPos = true;
}

bool SMsgWriter::writeLEDState()
{
  if (!cp->supportsLEDState)
    return false;
  if (cp->ledState() == ledUnknown)
    return false;

  needLEDState = true;

  return true;
}

bool SMsgWriter::writeQEMUKeyEvent()
{
  if (!cp->supportsQEMUKeyEvent)
    return false;

  needQEMUKeyEvent = true;

  return true;
}

bool SMsgWriter::needFakeUpdate()
{
  if (needSetDesktopName)
    return true;
  if (needSetCursor || needSetXCursor || needSetCursorWithAlpha || needSetVMWareCursor)
    return true;
  if (needCursorPos)
    return true;
  if (needLEDState)
    return true;
  if (needQEMUKeyEvent)
    return true;
  if (needNoDataUpdate())
    return true;

  return false;
}

bool SMsgWriter::needNoDataUpdate()
{
  if (needSetDesktopSize)
    return true;
  if (needExtendedDesktopSize || !extendedDesktopSizeMsgs.empty())
    return true;

  return false;
}

void SMsgWriter::writeNoDataUpdate()
{
  int nRects;

  nRects = 0;

  if (needSetDesktopSize)
    nRects++;
  if (needExtendedDesktopSize)
    nRects++;
  if (!extendedDesktopSizeMsgs.empty())
    nRects += extendedDesktopSizeMsgs.size();

  writeFramebufferUpdateStart(nRects);
  writeNoDataRects();
  writeFramebufferUpdateEnd();
}

void SMsgWriter::writeFramebufferUpdateStart(int nRects)
{
  startMsg(msgTypeFramebufferUpdate);
  os->pad(1);

  if (nRects != 0xFFFF) {
    if (needSetDesktopName)
      nRects++;
    if (needSetCursor)
      nRects++;
    if (needSetXCursor)
      nRects++;
    if (needSetCursorWithAlpha)
      nRects++;
    if (needSetVMWareCursor)
      nRects++;
    if (needCursorPos)
      nRects++;
    if (needLEDState)
      nRects++;
    if (needQEMUKeyEvent)
      nRects++;
  }

  os->writeU16(nRects);

  nRectsInUpdate = dataRectsInUpdate = 0;
  if (nRects == 0xFFFF)
    nRectsInHeader = 0;
  else
    nRectsInHeader = nRects;

  writePseudoRects();
}

void SMsgWriter::writeFramebufferUpdateEnd()
{
  if (nRectsInUpdate != nRectsInHeader && nRectsInHeader)
    throw Exception("SMsgWriter::writeFramebufferUpdateEnd: "
                    "nRects out of sync");

  if (nRectsInHeader == 0) {
    // Send last rect. marker
    os->writeS16(0);
    os->writeS16(0);
    os->writeU16(0);
    os->writeU16(0);
    os->writeU32(pseudoEncodingLastRect);

    // Send an UDP flip marker, if needed
    if (cp->supportsUdp) {
      udps->writeS16(dataRectsInUpdate);
      udps->writeS16(0);
      udps->writeU16(0);
      udps->writeU16(0);
      udps->writeU32(pseudoEncodingLastRect);
      udps->flush();
    }
  }

  endMsg();
}

void SMsgWriter::writeCopyRect(const Rect& r, int srcX, int srcY)
{
  startRect(r,encodingCopyRect);
  if (cp->supportsUdp) {
    udps->writeU16(srcX);
    udps->writeU16(srcY);
  } else {
    os->writeU16(srcX);
    os->writeU16(srcY);
  }
  endRect();
}

void SMsgWriter::startRect(const Rect& r, int encoding)
{
  if (++nRectsInUpdate > nRectsInHeader && nRectsInHeader)
    throw Exception("SMsgWriter::startRect: nRects out of sync");
  ++dataRectsInUpdate;

  if (cp->supportsUdp) {
    udps->writeS16(r.tl.x);
    udps->writeS16(r.tl.y);
    udps->writeU16(r.width());
    udps->writeU16(r.height());
    udps->writeU32(encoding);
  } else {
    os->writeS16(r.tl.x);
    os->writeS16(r.tl.y);
    os->writeU16(r.width());
    os->writeU16(r.height());
    os->writeU32(encoding);
  }
}

void SMsgWriter::endRect()
{
  if (cp->supportsUdp)
    udps->flush();
  else
    os->flush();
}

void SMsgWriter::startMsg(int type)
{
  os->writeU8(type);
}

void SMsgWriter::endMsg()
{
  os->flush();
}

void SMsgWriter::writePseudoRects()
{
  if (needSetCursor) {
    const Cursor& cursor = cp->cursor();

    rdr::U8Array data(cursor.width()*cursor.height() * (cp->pf().bpp/8));
    rdr::U8Array mask(cursor.getMask());

    const rdr::U8* in;
    rdr::U8* out;

    in = cursor.getBuffer();
    out = data.buf;
    for (int i = 0;i < cursor.width()*cursor.height();i++) {
      cp->pf().bufferFromRGB(out, in, 1);
      in += 4;
      out += cp->pf().bpp/8;
    }

    writeSetCursorRect(cursor.width(), cursor.height(),
                       cursor.hotspot().x, cursor.hotspot().y,
                       data.buf, mask.buf);
    needSetCursor = false;
  }

  if (needSetXCursor) {
    const Cursor& cursor = cp->cursor();
    rdr::U8Array bitmap(cursor.getBitmap());
    rdr::U8Array mask(cursor.getMask());

    writeSetXCursorRect(cursor.width(), cursor.height(),
                        cursor.hotspot().x, cursor.hotspot().y,
                        bitmap.buf, mask.buf);
    needSetXCursor = false;
  }

  if (needSetCursorWithAlpha) {
    const Cursor& cursor = cp->cursor();

    writeSetCursorWithAlphaRect(cursor.width(), cursor.height(),
                                cursor.hotspot().x, cursor.hotspot().y,
                                cursor.getBuffer());
    needSetCursorWithAlpha = false;
  }

  if (needSetVMWareCursor) {
    const Cursor& cursor = cp->cursor();

    writeSetVMwareCursorRect(cursor.width(), cursor.height(),
                                cursor.hotspot().x, cursor.hotspot().y,
                                cursor.getBuffer());
    needSetVMWareCursor = false;
  }

  if (needCursorPos) {
    const Point& cursorPos = cp->cursorPos();

    if (cp->supportsEncoding(pseudoEncodingVMwareCursorPosition)) {
      writeSetVMwareCursorPositionRect(cursorPos.x, cursorPos.y);
    } else {
      throw Exception("Client does not support cursor position");
    }

    needCursorPos = false;
  }

  if (needSetDesktopName) {
    writeSetDesktopNameRect(cp->name());
    needSetDesktopName = false;
  }

  if (needLEDState) {
    writeLEDStateRect(cp->ledState());
    needLEDState = false;
  }

  if (needQEMUKeyEvent) {
    writeQEMUKeyEventRect();
    needQEMUKeyEvent = false;
  }
}

void SMsgWriter::writeNoDataRects()
{
  // Start with specific ExtendedDesktopSize messages
  if (!extendedDesktopSizeMsgs.empty()) {
    std::list<ExtendedDesktopSizeMsg>::const_iterator ri;

    for (ri = extendedDesktopSizeMsgs.begin();ri != extendedDesktopSizeMsgs.end();++ri) {
      writeExtendedDesktopSizeRect(ri->reason, ri->result,
                                   ri->fb_width, ri->fb_height, ri->layout);
    }

    extendedDesktopSizeMsgs.clear();
  }

  // Send this before SetDesktopSize to make life easier on the clients
  if (needExtendedDesktopSize) {
    writeExtendedDesktopSizeRect(0, 0, cp->width, cp->height,
                                 cp->screenLayout);
    needExtendedDesktopSize = false;
  }

  // Some clients assume this is the last rectangle so don't send anything
  // more after this
  if (needSetDesktopSize) {
    writeSetDesktopSizeRect(cp->width, cp->height);
    needSetDesktopSize = false;
  }
}

void SMsgWriter::writeSetDesktopSizeRect(int width, int height)
{
  if (!cp->supportsDesktopResize)
    throw Exception("Client does not support desktop resize");
  if (++nRectsInUpdate > nRectsInHeader && nRectsInHeader)
    throw Exception("SMsgWriter::writeSetDesktopSizeRect: nRects out of sync");

  os->writeS16(0);
  os->writeS16(0);
  os->writeU16(width);
  os->writeU16(height);
  os->writeU32(pseudoEncodingDesktopSize);
}

void SMsgWriter::writeExtendedDesktopSizeRect(rdr::U16 reason,
                                              rdr::U16 result,
                                              int fb_width,
                                              int fb_height,
                                              const ScreenSet& layout)
{
  ScreenSet::const_iterator si;

  if (!cp->supportsExtendedDesktopSize)
    throw Exception("Client does not support extended desktop resize");
  if (++nRectsInUpdate > nRectsInHeader && nRectsInHeader)
    throw Exception("SMsgWriter::writeExtendedDesktopSizeRect: nRects out of sync");

  os->writeU16(reason);
  os->writeU16(result);
  os->writeU16(fb_width);
  os->writeU16(fb_height);
  os->writeU32(pseudoEncodingExtendedDesktopSize);

  os->writeU8(layout.num_screens());
  os->pad(3);

  for (si = layout.begin();si != layout.end();++si) {
    os->writeU32(si->id);
    os->writeU16(si->dimensions.tl.x);
    os->writeU16(si->dimensions.tl.y);
    os->writeU16(si->dimensions.width());
    os->writeU16(si->dimensions.height());
    os->writeU32(si->flags);
  }
}

void SMsgWriter::writeSetDesktopNameRect(const char *name)
{
  if (!cp->supportsDesktopRename)
    throw Exception("Client does not support desktop rename");
  if (++nRectsInUpdate > nRectsInHeader && nRectsInHeader)
    throw Exception("SMsgWriter::writeSetDesktopNameRect: nRects out of sync");

  os->writeS16(0);
  os->writeS16(0);
  os->writeU16(0);
  os->writeU16(0);
  os->writeU32(pseudoEncodingDesktopName);
  os->writeString(name);
}

void SMsgWriter::writeSetCursorRect(int width, int height,
                                    int hotspotX, int hotspotY,
                                    const void* data, const void* mask)
{
  if (!cp->supportsLocalCursor)
    throw Exception("Client does not support local cursors");
  if (++nRectsInUpdate > nRectsInHeader && nRectsInHeader)
    throw Exception("SMsgWriter::writeSetCursorRect: nRects out of sync");

  os->writeS16(hotspotX);
  os->writeS16(hotspotY);
  os->writeU16(width);
  os->writeU16(height);
  os->writeU32(pseudoEncodingCursor);
  os->writeBytes(data, width * height * (cp->pf().bpp/8));
  os->writeBytes(mask, (width+7)/8 * height);
}

void SMsgWriter::writeSetXCursorRect(int width, int height,
                                     int hotspotX, int hotspotY,
                                     const void* data, const void* mask)
{
  if (!cp->supportsLocalXCursor)
    throw Exception("Client does not support local cursors");
  if (++nRectsInUpdate > nRectsInHeader && nRectsInHeader)
    throw Exception("SMsgWriter::writeSetXCursorRect: nRects out of sync");

  os->writeS16(hotspotX);
  os->writeS16(hotspotY);
  os->writeU16(width);
  os->writeU16(height);
  os->writeU32(pseudoEncodingXCursor);
  if (width * height > 0) {
    os->writeU8(255);
    os->writeU8(255);
    os->writeU8(255);
    os->writeU8(0);
    os->writeU8(0);
    os->writeU8(0);
    os->writeBytes(data, (width+7)/8 * height);
    os->writeBytes(mask, (width+7)/8 * height);
  }
}

void SMsgWriter::writeSetCursorWithAlphaRect(int width, int height,
                                             int hotspotX, int hotspotY,
                                             const rdr::U8* data)
{
  if (!cp->supportsLocalCursorWithAlpha)
    throw Exception("Client does not support local cursors");
  if (++nRectsInUpdate > nRectsInHeader && nRectsInHeader)
    throw Exception("SMsgWriter::writeSetCursorWithAlphaRect: nRects out of sync");

  os->writeS16(hotspotX);
  os->writeS16(hotspotY);
  os->writeU16(width);
  os->writeU16(height);
  os->writeU32(pseudoEncodingCursorWithAlpha);

  // FIXME: Use an encoder with compression?
  os->writeU32(encodingRaw);

  // Alpha needs to be pre-multiplied
  for (int i = 0;i < width*height;i++) {
    os->writeU8((unsigned)data[0] * data[3] / 255);
    os->writeU8((unsigned)data[1] * data[3] / 255);
    os->writeU8((unsigned)data[2] * data[3] / 255);
    os->writeU8(data[3]);
    data += 4;
  }
}

void SMsgWriter::writeSetVMwareCursorRect(int width, int height,
                                          int hotspotX, int hotspotY,
                                          const rdr::U8* data)
{
  if (!cp->supportsVMWareCursor)
    throw Exception("Client does not support local cursors");
  if (++nRectsInUpdate > nRectsInHeader && nRectsInHeader)
    throw Exception("SMsgWriter::writeSetVMwareCursorRect: nRects out of sync");

  os->writeS16(hotspotX);
  os->writeS16(hotspotY);
  os->writeU16(width);
  os->writeU16(height);
  os->writeU32(pseudoEncodingVMwareCursor);

  os->writeU8(1); // Alpha cursor
  os->pad(1);

  // FIXME: Should alpha be premultiplied?
  os->writeBytes(data, width*height*4);
}

void SMsgWriter::writeSetVMwareCursorPositionRect(int hotspotX, int hotspotY)
{
  if (!cp->supportsEncoding(pseudoEncodingVMwareCursorPosition))
    throw Exception("Client does not support cursor position");
  if (++nRectsInUpdate > nRectsInHeader && nRectsInHeader)
    throw Exception("SMsgWriter::writeSetVMwareCursorRect: nRects out of sync");

  os->writeS16(hotspotX);
  os->writeS16(hotspotY);
  os->writeU16(0);
  os->writeU16(0);
  os->writeU32(pseudoEncodingVMwareCursorPosition);
}

void SMsgWriter::writeLEDStateRect(rdr::U8 state)
{
  if (!cp->supportsLEDState)
    throw Exception("Client does not support LED state updates");
  if (cp->ledState() == ledUnknown)
    throw Exception("Server does not support LED state updates");
  if (++nRectsInUpdate > nRectsInHeader && nRectsInHeader)
    throw Exception("SMsgWriter::writeLEDStateRect: nRects out of sync");

  os->writeS16(0);
  os->writeS16(0);
  os->writeU16(0);
  os->writeU16(0);
  os->writeU32(pseudoEncodingLEDState);
  os->writeU8(state);
}

void SMsgWriter::writeQEMUKeyEventRect()
{
  if (!cp->supportsQEMUKeyEvent)
    throw Exception("Client does not support QEMU extended key events");
  if (++nRectsInUpdate > nRectsInHeader && nRectsInHeader)
    throw Exception("SMsgWriter::writeQEMUKeyEventRect: nRects out of sync");

  os->writeS16(0);
  os->writeS16(0);
  os->writeU16(0);
  os->writeU16(0);
  os->writeU32(pseudoEncodingQEMUKeyEvent);
}

void SMsgWriter::writeUdpUpgrade(const char *resp)
{
  startMsg(msgTypeUpgradeToUdp);

  rdr::U16 len = strlen(resp);
  os->writeU16(len);
  os->writeBytes(resp, len);

  endMsg();
}

void SMsgWriter::writeSubscribeUnixRelay(const bool success, const char *msg)
{
  startMsg(msgTypeSubscribeUnixRelay);

  const rdr::U8 len = strlen(msg);
  os->writeU8(success);
  os->writeU8(len);
  os->writeBytes(msg, len);

  endMsg();
}

void SMsgWriter::writeUnixRelay(const char *name, const rdr::U8 *buf, const unsigned len)
{
  startMsg(msgTypeUnixRelay);

  const rdr::U8 namelen = strlen(name);
  os->writeU8(namelen);
  os->writeBytes(name, namelen);

  os->writeU32(len);
  os->writeBytes(buf, len);

  endMsg();
}

void SMsgWriter::writeUserJoinedSession(const std::string& username)
{
  startMsg(msgTypeUserAddedToSession);
  os->writeString(username.c_str());
  endMsg();
}

void SMsgWriter::writeUserLeftSession(const std::string& username)
{
  startMsg(msgTypeUserRemovedFromSession);
  os->writeString(username.c_str());
  endMsg();
}
