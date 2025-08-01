/* Copyright (C) 2002-2005 RealVNC Ltd.  All Rights Reserved.
 * Copyright 2009-2018 Pierre Ossman for Cendio AB
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
 
#include <network/GetAPI.h>
#include <network/TcpSocket.h>

#include <rfb/ComparingUpdateTracker.h>
#include <rfb/Encoder.h>
#include <rfb/KeyRemapper.h>
#include <rfb/LogWriter.h>
#include <rfb/Security.h>
#include <rfb/ServerCore.h>
#include <rfb/SMsgWriter.h>
#include <rfb/VNCServerST.h>
#include <rfb/VNCSConnectionST.h>
#include <rfb/screenTypes.h>
#include <rfb/fenceTypes.h>
#include <rfb/ledStates.h>
#define XK_LATIN1
#define XK_MISCELLANY
#define XK_XKB_KEYS
#include <rfb/keysymdef.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdint.h>
#include <wordexp.h>

#include "kasmpasswd.h"

using namespace rfb;

static LogWriter vlog("VNCSConnST");

static Cursor emptyCursor(0, 0, Point(0, 0), NULL);

extern rfb::BoolParameter disablebasicauth;

extern "C" char unixrelaynames[MAX_UNIX_RELAYS][MAX_UNIX_RELAY_NAME_LEN];

VNCSConnectionST::VNCSConnectionST(VNCServerST* server_, network::Socket *s,
                                   bool reverse)
  : upgradingToUdp(false), sock(s), reverseConnection(reverse),
    inProcessMessages(false),
    pendingSyncFence(false), syncFence(false), fenceFlags(0),
    fenceDataLen(0), fenceData(NULL), congestionTimer(this),
    losslessTimer(this), kbdLogTimer(this), binclipTimer(this),
    server(server_), updates(false),
    updateRenderedCursor(false), removeRenderedCursor(false),
    continuousUpdates(false), encodeManager(this, &VNCServerST::encCache),
    needsPermCheck(false), pointerEventTime(0),
    clientHasCursor(false),
    accessRights(AccessDefault), startTime(time(0)), frameTracking(false),
    udpFramesSinceFull(0), complainedAboutNoViewRights(false), clientUsername("username_unavailable")
{
  setStreams(&sock->inStream(), &sock->outStream());
  peerEndpoint.buf = sock->getPeerEndpoint();
  VNCServerST::connectionsLog.write(1,"accepted: %s", peerEndpoint.buf);

  memset(bstats_total, 0, sizeof(bstats_total));
  gettimeofday(&connStart, NULL);

  unsigned i;
  for (i = 0; i < MAX_UNIX_RELAYS; i++)
    unixRelaySubscriptions[i][0] = '\0';

  // Check their permissions, if applicable
  kasmpasswdpath[0] = '\0';
  wordexp_t wexp;
  if (!wordexp(rfb::Server::kasmPasswordFile, &wexp, WRDE_NOCMD))
    strncpy(kasmpasswdpath, wexp.we_wordv[0], 4096);
  kasmpasswdpath[4095] = '\0';
  wordfree(&wexp);

  user[0] = '\0';
  const char *at = strrchr(peerEndpoint.buf, '@');
  if (at && at - peerEndpoint.buf > 1 && at - peerEndpoint.buf < USERNAME_LEN) {
    memcpy(user, peerEndpoint.buf, at - peerEndpoint.buf);
    user[at - peerEndpoint.buf] = '\0';
  }

  bool read, write, owner;
  if (!getPerms(read, write, owner)) {
    accessRights &= ~(WRITER_PERMS | AccessView);
  }
  if (!write) {
    accessRights &= ~WRITER_PERMS;
  }
  if (!read) {
    accessRights &= ~AccessView;
  }

  // Configure the socket
  setSocketTimeouts();
  lastEventTime = time(0);
  gettimeofday(&lastRealUpdate, NULL);
  gettimeofday(&lastClipboardOp, NULL);
  gettimeofday(&lastKeyEvent, NULL);

  server->clients.push_front(this);

  if (server->apimessager)
    server->apimessager->mainUpdateUserInfo(checkOwnerConn(), server->clients.size());
}


VNCSConnectionST::~VNCSConnectionST()
{
  // If we reach here then VNCServerST is deleting us!
  VNCServerST::connectionsLog.write(1,"closed: %s (%s)",
                                    peerEndpoint.buf,
                                    (closeReason.buf) ? closeReason.buf : "");

  // Release any keys the client still had pressed
  while (!pressedKeys.empty()) {
    rdr::U32 keysym, keycode;

    keysym = pressedKeys.begin()->second;
    keycode = pressedKeys.begin()->first;
    pressedKeys.erase(pressedKeys.begin());

    vlog.debug("Releasing key 0x%x / 0x%x on client disconnect",
               keysym, keycode);
    server->desktop->keyEvent(keysym, keycode, false);
  }

  if (server->pointerClient == this)
    server->pointerClient = 0;

  // Remove this client from the server
  server->clients.remove(this);

  delete [] fenceData;

  if (server->apimessager) {
    server->apimessager->mainUpdateUserInfo(checkOwnerConn(), server->clients.size());
    server->apimessager->mainClearBottleneckStats(peerEndpoint.buf);
  }
}


// Methods called from VNCServerST

bool VNCSConnectionST::init()
{
  try {
    initialiseProtocol();
  } catch (rdr::Exception& e) {
    close(e.str());
    return false;
  }
  return true;
}

void VNCSConnectionST::close(const char* reason)
{
  // Log the reason for the close
  if (!closeReason.buf)
    closeReason.buf = strDup(reason);
  else
    vlog.debug("second close: %s (%s)", peerEndpoint.buf, reason);

  if (authenticated()) {
      server->lastDisconnectTime = time(0);

      // First update the client state to CLOSING to ensure it's not included in user lists
      setState(RFBSTATE_CLOSING);

      // Notify other clients about the user leaving
      server->notifyUserAction(this, clientUsername, VNCServerST::Leave);
      vlog.info("Notifying other clients that user '%s' left: %s",
                clientUsername.c_str(), reason ? reason : "connection closed");

      if (server->apimessager)
      {
        server->updateSessionUsersList();
      }
  }

  try {
    if (sock->outStream().bufferUsage() > 0) {
      sock->cork(false);
      sock->outStream().flush();
      if (sock->outStream().bufferUsage() > 0)
        vlog.error("Failed to flush remaining socket data on close");
    }
  } catch (rdr::Exception& e) {
    vlog.error("Failed to flush remaining socket data on close: %s", e.str());
  }

  // Just shutdown the socket and mark our state as closing if not already done.
  // Eventually the calling code will call VNCServerST's removeSocket() method
  // causing us to be deleted.
  sock->shutdown();
  if (state() != RFBSTATE_CLOSING)
    setState(RFBSTATE_CLOSING);
}


void VNCSConnectionST::processMessages()
{
  if (state() == RFBSTATE_CLOSING) return;
  try {
    // - Now set appropriate socket timeouts and process data
    setSocketTimeouts();

    inProcessMessages = true;

    // Get the underlying TCP layer to build large packets if we send
    // multiple small responses.
    sock->cork(true);

    while (getInStream()->checkNoWait(1)) {
      if (pendingSyncFence) {
        syncFence = true;
        pendingSyncFence = false;
      }

      processMsg();

      if (syncFence) {
        writer()->writeFence(fenceFlags, fenceDataLen, fenceData);
        syncFence = false;
      }
    }

    // Flush out everything in case we go idle after this.
    sock->cork(false);

    inProcessMessages = false;

    // If there were anything requiring an update, try to send it here.
    // We wait until now with this to aggregate responses and to give 
    // higher priority to user actions such as keyboard and pointer events.
    writeFramebufferUpdate();
  } catch (rdr::EndOfStream&) {
    close("Clean disconnection");
  } catch (rdr::Exception &e) {
    close(e.str());
  }
}

void VNCSConnectionST::flushSocket()
{
  if (state() == RFBSTATE_CLOSING) return;
  try {
    setSocketTimeouts();
    sock->outStream().flush();
    // Flushing the socket might release an update that was previously
    // delayed because of congestion.
    if (sock->outStream().bufferUsage() == 0)
      writeFramebufferUpdate();
  } catch (rdr::Exception &e) {
    close(e.str());
  }
}

void VNCSConnectionST::pixelBufferChange()
{
  try {
    if (!authenticated()) return;
    if (cp.width && cp.height && (server->pb->width() != cp.width ||
                                  server->pb->height() != cp.height))
    {
      // We need to clip the next update to the new size, but also add any
      // extra bits if it's bigger.  If we wanted to do this exactly, something
      // like the code below would do it, but at the moment we just update the
      // entire new size.  However, we do need to clip the damagedCursorRegion
      // because that might be added to updates in writeFramebufferUpdate().

      //updates.intersect(server->pb->getRect());
      //
      //if (server->pb->width() > cp.width)
      //  updates.add_changed(Rect(cp.width, 0, server->pb->width(),
      //                           server->pb->height()));
      //if (server->pb->height() > cp.height)
      //  updates.add_changed(Rect(0, cp.height, cp.width,
      //                           server->pb->height()));

      damagedCursorRegion.assign_intersect(server->pb->getRect());

      cp.width = server->pb->width();
      cp.height = server->pb->height();
      cp.screenLayout = server->screenLayout;
      if (state() == RFBSTATE_NORMAL) {
        // We should only send EDS to client asking for both
        if (!writer()->writeExtendedDesktopSize()) {
          if (!writer()->writeSetDesktopSize()) {
            close("Client does not support desktop resize");
            return;
          }
        }
      }

      // Drop any lossy tracking that is now outside the framebuffer
      encodeManager.pruneLosslessRefresh(Region(server->pb->getRect()));
    }
    // Just update the whole screen at the moment because we're too lazy to
    // work out what's actually changed.
    updates.clear();
    updates.add_changed(server->pb->getRect());
    writeFramebufferUpdate();
  } catch(rdr::Exception &e) {
    close(e.str());
  }
}

void VNCSConnectionST::writeFramebufferUpdateOrClose()
{
  try {
    writeFramebufferUpdate();
  } catch(rdr::Exception &e) {
    close(e.str());
  }
}

void VNCSConnectionST::screenLayoutChangeOrClose(rdr::U16 reason)
{
  try {
    screenLayoutChange(reason);
    writeFramebufferUpdate();
  } catch(rdr::Exception &e) {
    close(e.str());
  }
}

void VNCSConnectionST::bellOrClose()
{
  try {
    if (state() == RFBSTATE_NORMAL) writer()->writeBell();
  } catch(rdr::Exception& e) {
    close(e.str());
  }
}

char *percentEncode(const char *str, const unsigned len) {
  char *enc = (char *) calloc(len * 3 + 1, 1);
  char *out = enc;
  unsigned i;

  for (i = 0; i < len; i++) {
    if (isalnum(str[i]) || str[i] == ' ' || str[i] == '.' || str[i] == ',' ||
        str[i] == '?' || str[i] == '!' || str[i] == '"' || str[i] == '\'') {
      *out++ = str[i];
    } else {
      *out++ = '%';
      sprintf(out, "%02X", str[i]);
      out += 2;
    }
  }

  return enc;
}

char *percentEncode4(const uint16_t *str, const unsigned len) {
  char *enc = (char *) calloc(len * 5 + 1, 1);
  char *out = enc;
  unsigned i;

  for (i = 0; i < len; i++) {
    if ((str[i] < 128 && isalnum(str[i])) || str[i] == ' ' || str[i] == '.' ||
        str[i] == ',' ||
        str[i] == '?' || str[i] == '!' || str[i] == '"' || str[i] == '\'') {
      *out++ = str[i];
    } else {
      *out++ = '%';
      sprintf(out, "%04hX", str[i]);
      out += 4;
    }
  }

  return enc;
}

static void cliplog(const char *str, const int len, const int origlen,
                    const char *dir, const char *client, const unsigned id) {
  if (Server::DLP_ClipLog[0] == 'o')
    return;
  if (Server::DLP_ClipLog[0] == 'i') {
    vlog.info("DLP: client %s: %s %u (%u requested) clipboard bytes, id %u", client, dir,
              len, origlen, id);
  } else {
    // URL-encode it
    char *enc = percentEncode(str, len);

    vlog.info("DLP: client %s: %s %u (%u requested) clipboard bytes, id %u: '%s'",
              client, dir, len, origlen, id, enc);
    free(enc);
  }
}

#define KEYBUF_MAX 100
static uint16_t keybuf[KEYBUF_MAX];
static unsigned keybuf_cur;

static void flushKeylog(const char *client) {

  if (Server::DLP_ClipLog[0] != 'v' || !keybuf_cur)
    return;

  char *enc = percentEncode4(keybuf, keybuf_cur);

  vlog.info("DLP: client %s: keyboard bytes: '%s'",
            client, enc);
  free(enc);

  keybuf_cur = 0;
}

static void keylog(unsigned keysym, const char *client) {

  if (Server::DLP_ClipLog[0] != 'v')
    return;

  bool flush = false;
  if (keysym == XK_Return)
    flush = true;

  // Map over-16bit keys to 0xffff - most eastern is under that
  if (keysym > 0xffff)
    keysym = 0xffff;

  keybuf[keybuf_cur] = keysym;
  keybuf_cur++;

  if (keybuf_cur >= KEYBUF_MAX || keysym == '\n' || flush)
    flushKeylog(client);
}

void VNCSConnectionST::announceClipboardOrClose(bool available)
{
  try {
    if (!(accessRights & AccessCutText)) return;
    if (!rfb::Server::sendCutText) return;
    if (state() != RFBSTATE_NORMAL) return;
    announceClipboard(available);
  } catch(rdr::Exception& e) {
    close(e.str());
  }
}

void VNCSConnectionST::clearBinaryClipboardData()
{
  clearBinaryClipboard();
}

void VNCSConnectionST::sendBinaryClipboardDataOrClose(const char* mime,
                                                      const unsigned char *data,
                                                      const unsigned len,
                                                      const unsigned id)
{
  try {
    if (!(accessRights & AccessCutText)) return;
    if (!rfb::Server::sendCutText) return;
    if (rfb::Server::DLP_ClipSendMax && len > (unsigned) rfb::Server::DLP_ClipSendMax) {
      vlog.info("DLP: client %s: refused to send binary clipboard, too large",
                sock->getPeerAddress());
      return;
    }

    cliplog((const char *) data, len, len, "sent", sock->getPeerAddress(),
            id);
    if (state() != RFBSTATE_NORMAL) return;

    addBinaryClipboard(mime, data, len, id);
    binclipTimer.start(100);
  } catch(rdr::Exception& e) {
    close(e.str());
  }
}

void VNCSConnectionST::getBinaryClipboardData(const char* mime, const unsigned char **data,
                                              unsigned *len)
{
  unsigned i;
  for (i = 0; i < binaryClipboard.size(); i++) {
    if (!strcmp(binaryClipboard[i].mime, mime)) {
      *data = &binaryClipboard[i].data[0];
      *len = binaryClipboard[i].data.size();
      return;
    }
  }

  vlog.error("Binary clipboard data for mime %s not found", mime);
  *data = (const unsigned char *) "";
  *len = 1;
}

void VNCSConnectionST::setDesktopNameOrClose(const char *name)
{
  try {
    setDesktopName(name);
    writeFramebufferUpdate();
  } catch(rdr::Exception& e) {
    close(e.str());
  }
}


void VNCSConnectionST::setCursorOrClose()
{
  try {
    setCursor();
    writeFramebufferUpdate();
  } catch(rdr::Exception& e) {
    close(e.str());
  }
}


void VNCSConnectionST::setLEDStateOrClose(unsigned int state)
{
  try {
    setLEDState(state);
    writeFramebufferUpdate();
  } catch(rdr::Exception& e) {
    close(e.str());
  }
}


int VNCSConnectionST::checkIdleTimeout()
{
  int idleTimeout = rfb::Server::idleTimeout;
  if (idleTimeout == 0) return 0;
  if (state() != RFBSTATE_NORMAL && idleTimeout < 15)
    idleTimeout = 15; // minimum of 15 seconds while authenticating
  time_t now = time(0);
  if (now < lastEventTime) {
    // Someone must have set the time backwards.  Set lastEventTime so that the
    // idleTimeout will count from now.
    vlog.info("Time has gone backwards - resetting idle timeout");
    lastEventTime = now;
  }
  int timeLeft = lastEventTime + idleTimeout - now;
  if (timeLeft < -60) {
    // Our callback is over a minute late - someone must have set the time
    // forwards.  Set lastEventTime so that the idleTimeout will count from
    // now.
    vlog.info("Time has gone forwards - resetting idle timeout");
    lastEventTime = now;
    return secsToMillis(idleTimeout);
  }
  if (timeLeft <= 0) {
    close("Idle timeout");
    return 0;
  }
  return secsToMillis(timeLeft);
}


bool VNCSConnectionST::getComparerState()
{
  // We interpret a low compression level as an indication that the client
  // wants to prioritise CPU usage over bandwidth, and hence disable the
  // comparing update tracker.
  return (cp.compressLevel == -1) || (cp.compressLevel > 1);
}


// renderedCursorChange() is called whenever the server-side rendered cursor
// changes shape or position.  It ensures that the next update will clean up
// the old rendered cursor and if necessary draw the new rendered cursor.

void VNCSConnectionST::renderedCursorChange()
{
  if (state() != RFBSTATE_NORMAL) return;
  // Are we switching between client-side and server-side cursor?
  if (clientHasCursor == needRenderedCursor())
    setCursorOrClose();
  bool hasRenderedCursor = !damagedCursorRegion.is_empty();
  if (hasRenderedCursor)
    removeRenderedCursor = true;
  if (needRenderedCursor()) {
    updateRenderedCursor = true;
    writeFramebufferUpdateOrClose();
  }
}

// cursorPositionChange() is called whenever the cursor has changed position by
// the server.  If the client supports being informed about these changes then
// it will arrange for the new cursor position to be sent to the client.

void VNCSConnectionST::cursorPositionChange()
{
  setCursorPos();
}

// needRenderedCursor() returns true if this client needs the server-side
// rendered cursor.  This may be because it does not support local cursor or
// because the current cursor position has not been set by this client.
// Unfortunately we can't know for sure when the current cursor position has
// been set by this client.  We guess that this is the case when the current
// cursor position is the same as the last pointer event from this client, or
// if it is a very short time since this client's last pointer event (up to a
// second).  [ Ideally we should do finer-grained timing here and make the time
// configurable, but I don't think it's that important. ]

bool VNCSConnectionST::needRenderedCursor()
{
  if (state() != RFBSTATE_NORMAL)
    return false;

  if (!cp.supportsLocalCursorWithAlpha &&
      !cp.supportsVMWareCursor &&
      !cp.supportsLocalCursor && !cp.supportsLocalXCursor)
    return true;
  if (!server->cursorPos.equals(pointerEventPos) &&
      (time(0) - pointerEventTime) > 0)
    return true;

  return false;
}


void VNCSConnectionST::approveConnectionOrClose(bool accept,
                                                const char* reason)
{
  try {
    approveConnection(accept, reason);
  } catch (rdr::Exception& e) {
    close(e.str());
  }
}



// -=- Callbacks from SConnection

void VNCSConnectionST::authSuccess()
{
  lastEventTime = time(0);
  connectionTime = time(0); // Record when the user connected
  vlog.info("User %s connected at %ld", clientUsername.c_str(), connectionTime);

  server->startDesktop();

  // - Set the connection parameters appropriately
  cp.width = server->pb->width();
  cp.height = server->pb->height();
  cp.screenLayout = server->screenLayout;
  cp.setName(server->getName());
  cp.setLEDState(server->ledState);
  
  // - Set the default pixel format
  cp.setPF(server->pb->getPF());
  char buffer[256];
  cp.pf().print(buffer, 256);
  vlog.info("Server default pixel format %s", buffer);

  // - Mark the entire display as "dirty"
  updates.add_changed(server->pb->getRect());
  startTime = time(nullptr);

  if (clientUsername.empty())
  {
    setUsername(get_default_name(sock->getPeerAddress()));
  }
    vlog.info("Authentication successful for user: %s", clientUsername.c_str());
}

void VNCSConnectionST::queryConnection(const char* userName)
{
  if (userName && strlen(userName) > 0) {
    setUsername(userName);
    vlog.info("Setting username for connection: %s", userName);
  } else {
    // Generate a default username based on connection info
    setUsername(get_default_name(sock->getPeerAddress()));

    vlog.info("Generated username: %s", clientUsername.c_str());
  }

  // - Authentication succeeded - clear from blacklist
  CharArray name; name.buf = sock->getPeerAddress();
  server->blHosts->clearBlackmark(name.buf);

  // - Special case to provide a more useful error message
  if (rfb::Server::neverShared && !rfb::Server::disconnectClients &&
    server->authClientCount() > 0) {
    approveConnection(false, "The server is already in use");
    return;
  }

  // - Does the client have the right to bypass the query?
  if (reverseConnection ||
      !(rfb::Server::queryConnect || sock->requiresQuery()) ||
      (accessRights & AccessNoQuery))
  {
    approveConnection(true);
    return;
  }

  // - Get the server to display an Accept/Reject dialog, if required
  //   If a dialog is displayed, the result will be PENDING, and the
  //   server will call approveConnection at a later time
  CharArray reason;
  VNCServerST::queryResult qr = server->queryConnection(sock, userName,
                                                        &reason.buf);
  if (qr == VNCServerST::PENDING)
    return;

  // - If server returns ACCEPT/REJECT then pass result to SConnection
  approveConnection(qr == VNCServerST::ACCEPT, reason.buf);
}

void VNCSConnectionST::clientInit(bool shared)
{
  lastEventTime = time(0);
  if (rfb::Server::alwaysShared || reverseConnection) shared = true;
  if (!(accessRights & AccessNonShared)) shared = true;
  if (rfb::Server::neverShared) shared = false;
  if (!shared) {
    if (rfb::Server::disconnectClients && (accessRights & AccessNonShared)) {
      // - Close all the other connected clients
      vlog.debug("non-shared connection - closing clients");
      server->closeClients("Non-shared connection requested", getSock());
    } else {
      // - Refuse this connection if there are existing clients, in addition to
      // this one
      if (server->authClientCount() > 1) {
        close("Server is already in use");
        return;
      }
    }
  }
  SConnection::clientInit(shared);
  if (shared && authenticated()) {
    server->notifyUserAction(this, clientUsername, VNCServerST::Join);
    vlog.info("Notifying other clients that user '%s' joined the shared session",
              clientUsername.c_str());
  }

  if (server->apimessager && authenticated()) {
   server->updateSessionUsersList();
  }
}

void VNCSConnectionST::setPixelFormat(const PixelFormat& pf)
{
  SConnection::setPixelFormat(pf);
  char buffer[256];
  pf.print(buffer, 256);
  vlog.info("Client pixel format %s", buffer);
  setCursor();
}

void VNCSConnectionST::pointerEvent(const Point& pos, const Point& abspos, int buttonMask, const bool skipClick, const bool skipRelease, int scrollX, int scrollY)
{
  pointerEventTime = lastEventTime = time(0);
  server->lastUserInputTime = lastEventTime;
  if (!(accessRights & AccessPtrEvents)) {
    // This particular event is lost, but it's a corner case - you removed write access
    // from yourself, then added it back. The intended use is for multiple clients,
    // where the leader removes and adds back access for others, not himself.
    recheckPerms();
    return;
  }
  if (!rfb::Server::acceptPointerEvents) return;
  if (!server->pointerClient || server->pointerClient == this) {
    Point newpos = pos;
    if (pos.x & 0x4000) {
      newpos.x &= ~0x4000;
      newpos.y &= ~0x4000;

      if (newpos.x & 0x8000) {
        newpos.x &= ~0x8000;
        newpos.x = -newpos.x;
      }
      if (newpos.y & 0x8000) {
        newpos.y &= ~0x8000;
        newpos.y = -newpos.y;
      }

      if (newpos.x < 0) {
        if (pointerEventPos.x + newpos.x >= 0)
          pointerEventPos.x += newpos.x;
        else
          pointerEventPos.x = 0;
      } else {
        pointerEventPos.x += newpos.x;
        if (pointerEventPos.x >= cp.width)
          pointerEventPos.x = cp.width;
      }

      if (newpos.y < 0) {
        if (pointerEventPos.y + newpos.y >= 0)
          pointerEventPos.y += newpos.y;
        else
          pointerEventPos.y = 0;
      } else {
        pointerEventPos.y += newpos.y;
        if (pointerEventPos.y >= cp.height)
          pointerEventPos.y = cp.height;
      }
    } else {
      pointerEventPos = pos;
    }

    if (buttonMask)
      server->pointerClient = this;
    else
      server->pointerClient = 0;

    bool skipclick = false, skiprelease = false;
    if (server->DLPRegion.enabled) {
      rdr::U16 x1, y1, x2, y2;
      server->translateDLPRegion(x1, y1, x2, y2);

      if (pos.x < x1 || pos.x >= x2 ||
          pos.y < y1 || pos.y >= y2) {

          if (!Server::DLP_RegionAllowClick)
            skipclick = true;
          if (!Server::DLP_RegionAllowRelease)
            skiprelease = true;
      }
    }

    server->desktop->pointerEvent(newpos, pointerEventPos, buttonMask, skipclick, skiprelease, scrollX, scrollY);
  }
}


class VNCSConnectionSTShiftPresser {
public:
  VNCSConnectionSTShiftPresser(SDesktop* desktop_)
    : desktop(desktop_), pressed(false) {}
  ~VNCSConnectionSTShiftPresser() {
    if (pressed) {
      vlog.debug("Releasing fake Shift_L");
      desktop->keyEvent(XK_Shift_L, 0, false);
    }
  }
  void press() {
    vlog.debug("Pressing fake Shift_L");
    desktop->keyEvent(XK_Shift_L, 0, true);
    pressed = true;
  }
  SDesktop* desktop;
  bool pressed;
};

// keyEvent() - record in the pressedKeys which keys were pressed.  Allow
// multiple down events (for autorepeat), but only allow a single up event.
void VNCSConnectionST::keyEvent(rdr::U32 keysym, rdr::U32 keycode, bool down) {
  rdr::U32 lookup;

  lastEventTime = time(0);
  server->lastUserInputTime = lastEventTime;
  if (!(accessRights & AccessKeyEvents)) return;
  if (!rfb::Server::acceptKeyEvents) return;
  if (Server::DLP_KeyRateLimit > 0 && down &&
      msSince(&lastKeyEvent) < (1000 / (unsigned) Server::DLP_KeyRateLimit)) {
    vlog.info("DLP: client %s: refused keyboard event, too soon (%u ms vs %u)",
               sock->getPeerAddress(), msSince(&lastKeyEvent),
               (1000 / (unsigned) Server::DLP_KeyRateLimit));
    return;
  }

  gettimeofday(&lastKeyEvent, NULL);

  if (down) {
    keylog(keysym, sock->getPeerAddress());
    kbdLogTimer.start(60 * 1000);
    if (Server::DLP_ClipLog[0] == 'v')
      vlog.debug("Key pressed: 0x%x / 0x%x", keysym, keycode);
  } else {
    if (Server::DLP_ClipLog[0] == 'v')
      vlog.debug("Key released: 0x%x / 0x%x", keysym, keycode);
  }

  // Remap the key if required
  if (server->keyRemapper) {
    rdr::U32 newkey;
    newkey = server->keyRemapper->remapKey(keysym);
    if (newkey != keysym) {
      vlog.debug("Key remapped to 0x%x", newkey);
      keysym = newkey;
    }
  }

  // Avoid lock keys if we don't know the server state
  if ((server->ledState == ledUnknown) &&
      ((keysym == XK_Caps_Lock) ||
       (keysym == XK_Num_Lock) ||
       (keysym == XK_Scroll_Lock))) {
    vlog.debug("Ignoring lock key (e.g. caps lock)");
    return;
  }

  // Lock key heuristics
  // (only for clients that do not support the LED state extension)
  if (!cp.supportsLEDState) {
    // Always ignore ScrollLock as we don't have a heuristic
    // for that
    if (keysym == XK_Scroll_Lock) {
      vlog.debug("Ignoring lock key (e.g. caps lock)");
      return;
    }

    if (down && (server->ledState != ledUnknown)) {
      // CapsLock synchronisation heuristic
      // (this assumes standard interaction between CapsLock the Shift
      // keys and normal characters)
      if (((keysym >= XK_A) && (keysym <= XK_Z)) ||
          ((keysym >= XK_a) && (keysym <= XK_z))) {
        bool uppercase, shift, lock;

        uppercase = (keysym >= XK_A) && (keysym <= XK_Z);
        shift = isShiftPressed();
        lock = server->ledState & ledCapsLock;

        if (lock == (uppercase == shift)) {
          vlog.debug("Inserting fake CapsLock to get in sync with client");
          server->desktop->keyEvent(XK_Caps_Lock, 0, true);
          server->desktop->keyEvent(XK_Caps_Lock, 0, false);
        }
      }

      // NumLock synchronisation heuristic
      // (this is more cautious because of the differences between Unix,
      // Windows and macOS)
      if (((keysym >= XK_KP_Home) && (keysym <= XK_KP_Delete)) ||
          ((keysym >= XK_KP_0) && (keysym <= XK_KP_9)) ||
          (keysym == XK_KP_Separator) || (keysym == XK_KP_Decimal)) {
        bool number, shift, lock;

        number = ((keysym >= XK_KP_0) && (keysym <= XK_KP_9)) ||
                  (keysym == XK_KP_Separator) || (keysym == XK_KP_Decimal);
        shift = isShiftPressed();
        lock = server->ledState & ledNumLock;

        if (shift) {
          // We don't know the appropriate NumLock state for when Shift
          // is pressed as it could be one of:
          //
          // a) A Unix client where Shift negates NumLock
          //
          // b) A Windows client where Shift only cancels NumLock
          //
          // c) A macOS client where Shift doesn't have any effect
          //
        } else if (lock == (number == shift)) {
          vlog.debug("Inserting fake NumLock to get in sync with client");
          server->desktop->keyEvent(XK_Num_Lock, 0, true);
          server->desktop->keyEvent(XK_Num_Lock, 0, false);
        }
      }
    }
  }

  // Turn ISO_Left_Tab into shifted Tab.
  VNCSConnectionSTShiftPresser shiftPresser(server->desktop);
  if (keysym == XK_ISO_Left_Tab) {
    if (!isShiftPressed())
      shiftPresser.press();
    keysym = XK_Tab;
  }

  // We need to be able to track keys, so generate a fake index when we
  // aren't given a keycode
  if (keycode == 0)
    lookup = 0x80000000 | keysym;
  else
    lookup = keycode;

  // We force the same keysym for an already down key for the
  // sake of sanity
  if (pressedKeys.find(lookup) != pressedKeys.end())
    keysym = pressedKeys[lookup];

  if (down) {
    pressedKeys[lookup] = keysym;
  } else {
    if (!pressedKeys.erase(lookup))
      return;
  }

  server->desktop->keyEvent(keysym, keycode, down);
}

void VNCSConnectionST::framebufferUpdateRequest(const Rect& r,bool incremental)
{
  Rect safeRect;

  if (!(accessRights & AccessView)) return;

  SConnection::framebufferUpdateRequest(r, incremental);

  // Check that the client isn't sending crappy requests
  if (!r.enclosed_by(Rect(0, 0, cp.width, cp.height))) {
    vlog.error("FramebufferUpdateRequest %dx%d at %d,%d exceeds framebuffer %dx%d",
               r.width(), r.height(), r.tl.x, r.tl.y, cp.width, cp.height);
    safeRect = r.intersect(Rect(0, 0, cp.width, cp.height));
  } else {
    safeRect = r;
  }

  // Just update the requested region.
  // Framebuffer update will be sent a bit later, see processMessages().
  Region reqRgn(safeRect);
  if (!incremental || !continuousUpdates)
    requested.assign_union(reqRgn);

  if (!incremental) {
    // Non-incremental update - treat as if area requested has changed
    updates.add_changed(reqRgn);

    // And send the screen layout to the client (which, unlike the
    // framebuffer dimensions, the client doesn't get during init)
    writer()->writeExtendedDesktopSize();

    // We do not send a DesktopSize since it only contains the
    // framebuffer size (which the client already should know) and
    // because some clients don't handle extra DesktopSize events
    // very well.
  }
}

void VNCSConnectionST::setDesktopSize(int fb_width, int fb_height,
                                      const ScreenSet& layout)
{
  unsigned int result;

  server->sendWatermark = true;

  if (!(accessRights & AccessSetDesktopSize)) goto justnotify;
  if (!rfb::Server::acceptSetDesktopSize) goto justnotify;

  // Don't bother the desktop with an invalid configuration
  if (!layout.validate(fb_width, fb_height)) {
    writer()->writeExtendedDesktopSize(reasonClient, resultInvalid,
                                       fb_width, fb_height, layout);
    return;
  }

  // FIXME: the desktop will call back to VNCServerST and an extra set
  // of ExtendedDesktopSize messages will be sent. This is okay
  // protocol-wise, but unnecessary.
  result = server->desktop->setScreenLayout(fb_width, fb_height, layout);

  writer()->writeExtendedDesktopSize(reasonClient, result,
                                     fb_width, fb_height, layout);

  // Only notify other clients on success
  if (result == resultSuccess) {
    if (server->screenLayout != layout)
        throw Exception("Desktop configured a different screen layout than requested");
    server->notifyScreenLayoutChange(this);
  }

  return;

  justnotify:
  writer()->writeExtendedDesktopSize(reasonClient, resultProhibited,
                                     server->pb->getRect().width(),
                                     server->pb->getRect().height(),
                                     server->screenLayout);
}

void VNCSConnectionST::fence(rdr::U32 flags, unsigned len, const char data[])
{
  rdr::U8 type;

  if (flags & fenceFlagRequest) {
    if (flags & fenceFlagSyncNext) {
      pendingSyncFence = true;

      fenceFlags = flags & (fenceFlagBlockBefore | fenceFlagBlockAfter | fenceFlagSyncNext);
      fenceDataLen = len;
      delete [] fenceData;
      fenceData = NULL;
      if (len > 0) {
        fenceData = new char[len];
        memcpy(fenceData, data, len);
      }

      return;
    }

    // We handle everything synchronously so we trivially honor these modes
    flags = flags & (fenceFlagBlockBefore | fenceFlagBlockAfter);

    writer()->writeFence(flags, len, data);
    return;
  }

  if (len < 1)
    vlog.error("Fence response of unexpected size received");

  type = data[0];

  switch (type) {
  case 0:
    // Initial dummy fence;
    break;
  case 1:
    congestion.gotPong();
    break;
  default:
    vlog.error("Fence response of unexpected type received");
  }
}

void VNCSConnectionST::enableContinuousUpdates(bool enable,
                                               int x, int y, int w, int h)
{
  Rect rect;

  if (!cp.supportsFence || !cp.supportsContinuousUpdates)
    throw Exception("Client tried to enable continuous updates when not allowed");

  continuousUpdates = enable;

  rect.setXYWH(x, y, w, h);
  cuRegion.reset(rect);

  if (enable) {
    requested.clear();
  } else {
    writer()->writeEndOfContinuousUpdates();
  }
}

void VNCSConnectionST::handleClipboardAnnounce(bool available)
{
  if (!(accessRights & AccessCutText)) return;
  if (!rfb::Server::acceptCutText) return;
  server->handleClipboardAnnounce(this, available);
}

void VNCSConnectionST::handleClipboardAnnounceBinary(const unsigned num, const char mimes[][32])
{
  if (!(accessRights & AccessCutText)) return;
  if (!rfb::Server::acceptCutText) return;
  server->handleClipboardAnnounceBinary(this, num, mimes);

  const unsigned tolog = server->clipboardId++;

  if (Server::DLP_ClipLog[0] == 'o')
    return;
  vlog.info("DLP: client %s: %s %u clipboard mimes, id %u",
            sock->getPeerAddress(), "received",
            num, tolog);
}

// supportsLocalCursor() is called whenever the status of
// cp.supportsLocalCursor has changed.  If the client does now support local
// cursor, we make sure that the old server-side rendered cursor is cleaned up
// and the cursor is sent to the client.

void VNCSConnectionST::supportsLocalCursor()
{
  bool hasRenderedCursor = !damagedCursorRegion.is_empty();
  if (hasRenderedCursor && !needRenderedCursor())
    removeRenderedCursor = true;
  setCursor();
}

void VNCSConnectionST::supportsFence()
{
  char type = 0;
  writer()->writeFence(fenceFlagRequest, sizeof(type), &type);
}

void VNCSConnectionST::supportsContinuousUpdates()
{
  // We refuse to use continuous updates if we cannot monitor the buffer
  // usage using fences.
  if (!cp.supportsFence)
    return;

  writer()->writeEndOfContinuousUpdates();
}

void VNCSConnectionST::supportsLEDState()
{
  writer()->writeLEDState();
}


bool VNCSConnectionST::handleTimeout(Timer* t)
{
  try {
    if ((t == &congestionTimer) ||
        (t == &losslessTimer))
      writeFramebufferUpdate();
    else if (t == &kbdLogTimer)
      flushKeylog(sock->getPeerAddress());
    else if (t == &binclipTimer)
      writeBinaryClipboard();
  } catch (rdr::Exception& e) {
    close(e.str());
  }

  return false;
}

bool VNCSConnectionST::isShiftPressed()
{
    std::map<rdr::U32, rdr::U32>::const_iterator iter;

    for (iter = pressedKeys.begin(); iter != pressedKeys.end(); ++iter) {
      if (iter->second == XK_Shift_L)
        return true;
      if (iter->second == XK_Shift_R)
        return true;
    }

  return false;
}

bool VNCSConnectionST::getPerms(bool &read, bool &write, bool &owner) const
{
  bool found = false;
  if (disablebasicauth) {
    // We're running without basicauth
    read = true;
    write = true;
    return true;
  }
  if (user[0]) {
    struct kasmpasswd_t *set = readkasmpasswd(kasmpasswdpath);
    unsigned i;
    for (i = 0; i < set->num; i++) {
      if (!strcmp(set->entries[i].user, user)) {
        read = set->entries[i].read;
        write = set->entries[i].write;
        owner = set->entries[i].owner;

        // Writer can always read
        if (write)
          read = true;

        found = true;
        break;
      }
    }

    free(set->entries);
    free(set);
  }

  return found;
}

void VNCSConnectionST::writeRTTPing()
{
  char type;

  if (!cp.supportsFence)
    return;

  congestion.updatePosition(sock->outStream().length());

  // We need to make sure any old update are already processed by the
  // time we get the response back. This allows us to reliably throttle
  // back on client overload, as well as network overload.
  type = 1;
  writer()->writeFence(fenceFlagRequest | fenceFlagBlockBefore,
                       sizeof(type), &type);

  congestion.sentPing();
}

bool VNCSConnectionST::isCongested()
{
  int eta;

  congestionTimer.stop();

  // Stuff still waiting in the send buffer?
  sock->outStream().flush();
  congestion.debugTrace("congestion-trace.csv", sock->getFd());
  if (sock->outStream().bufferUsage() > 0)
    return true;

  if (!cp.supportsFence || cp.supportsUdp)
    return false;

  congestion.updatePosition(sock->outStream().length());
  if (!congestion.isCongested())
    return false;

  eta = congestion.getUncongestedETA();
  if (eta >= 0)
    congestionTimer.start(eta);

  if (eta > 1000 / rfb::Server::frameRate) {
    struct timeval now;
    gettimeofday(&now, NULL);

    bstats[BS_NET_SLOW].push_back(now);
    bstats_total[BS_NET_SLOW]++;
  }

  return true;
}


void VNCSConnectionST::writeFramebufferUpdate()
{
  congestion.updatePosition(sock->outStream().length());
  encodeManager.clearEncodingTime();

  // We're in the middle of processing a command that's supposed to be
  // synchronised. Allowing an update to slip out right now might violate
  // that synchronisation.
  if (syncFence)
    return;

  // We try to aggregate responses, so don't send out anything whilst we
  // still have incoming messages. processMessages() will give us another
  // chance to run once things are idle.
  if (inProcessMessages)
    return;

  if (state() != RFBSTATE_NORMAL)
    return;
  if (requested.is_empty() && !continuousUpdates)
    return;

  // Check that we actually have some space on the link and retry in a
  // bit if things are congested.
  if (isCongested())
    return;

  // Check for permission changes?
  if (needsPermCheck) {
    needsPermCheck = false;

    bool read, write, owner, ret;
    ret = getPerms(read, write, owner);
    if (!ret) {
      close("User was deleted");
      return;
    }

    if (!write) {
      accessRights &= ~WRITER_PERMS;
    } else {
      accessRights |= WRITER_PERMS;
    }

    if (!read) {
      accessRights &= ~AccessView;
    } else {
      accessRights |= AccessView;
    }
  }

  if (!(accessRights & AccessView)) {
    if (!complainedAboutNoViewRights) {
      complainedAboutNoViewRights = true;
      vlog.error("User %s has no read permissions. If this is not intended, grant them permissions with kasmvncpasswd or via the API",
                 user);
    }
    return;
  }

  // Updates often consists of many small writes, and in continuous
  // mode, we will also have small fence messages around the update. We
  // need to aggregate these in order to not clog up TCP's congestion
  // window.
  sock->cork(true);

  if (frameTracking)
    writer()->writeRequestFrameStats();

  // First take care of any updates that cannot contain framebuffer data
  // changes.
  writeNoDataUpdate();

  // Then real data (if possible)
  writeDataUpdate();

  sock->cork(false);

  congestion.updatePosition(sock->outStream().length());

  struct timeval now;
  gettimeofday(&now, NULL);
  bstats[BS_FRAME].push_back(now);
  bstats_total[BS_FRAME]++;
}

void VNCSConnectionST::writeNoDataUpdate()
{
  if (!writer()->needNoDataUpdate())
    return;

  writer()->writeNoDataUpdate();

  // Make sure no data update is sent until next request
  requested.clear();
}

void VNCSConnectionST::writeDataUpdate()
{
  Region req, pending;
  UpdateInfo ui;
  bool needNewUpdateInfo;
  const RenderedCursor *cursor;
  size_t maxUpdateSize;

  updates.enable_copyrect(cp.useCopyRect);

  // See what the client has requested (if anything)
  if (continuousUpdates)
    req = cuRegion.union_(requested);
  else
    req = requested;

  if (req.is_empty())
    return;

  // Get any framebuffer changes we haven't yet been informed of
  pending = server->getPendingRegion();

  // Get the lists of updates. Prior to exporting the data to the `ui' object,
  // getUpdateInfo() will normalize the `updates' object such way that its
  // `changed' and `copied' regions would not intersect.
  updates.getUpdateInfo(&ui, req);
  needNewUpdateInfo = false;

  // If the previous position of the rendered cursor overlaps the source of the
  // copy, then when the copy happens the corresponding rectangle in the
  // destination will be wrong, so add it to the changed region.

  if (!ui.copied.is_empty() && !damagedCursorRegion.is_empty()) {
    Region bogusCopiedCursor;

    bogusCopiedCursor = damagedCursorRegion;
    bogusCopiedCursor.translate(ui.copy_delta);
    bogusCopiedCursor.assign_intersect(server->pb->getRect());
    if (!ui.copied.intersect(bogusCopiedCursor).is_empty()) {
      updates.add_changed(bogusCopiedCursor);
      needNewUpdateInfo = true;
    }
  }

  // If we need to remove the old rendered cursor, just add the region to
  // the changed region.

  if (removeRenderedCursor) {
    updates.add_changed(damagedCursorRegion);
    needNewUpdateInfo = true;
    damagedCursorRegion.clear();
    removeRenderedCursor = false;
  }

  // If we need a full cursor update then make sure its entire region
  // is marked as changed.

  if (updateRenderedCursor) {
    updates.add_changed(server->getRenderedCursor()->getEffectiveRect());
    needNewUpdateInfo = true;
    updateRenderedCursor = false;
  }

  // The `updates' object could change, make sure we have valid update info.

  if (needNewUpdateInfo)
    updates.getUpdateInfo(&ui, req);

  // If there are queued updates then we cannot safely send an update
  // without risking a partially updated screen

  if (!pending.is_empty()) {
    // However we might still be able to send a lossless refresh
    req.assign_subtract(pending);
    req.assign_subtract(ui.changed);
    req.assign_subtract(ui.copied);

    if (copypassed.size()) {
      Region everything;

      for (std::vector<CopyPassRect>::const_iterator it = copypassed.begin();
           it != copypassed.end(); it++) {
        everything.assign_union(it->rect);
      }

      req.assign_subtract(everything);
    }

    ui.changed.clear();
    ui.copied.clear();
  }

  // Does the client need a server-side rendered cursor?

  cursor = NULL;
  if (needRenderedCursor()) {
    Rect renderedCursorRect;

    cursor = server->getRenderedCursor();
    renderedCursorRect = cursor->getEffectiveRect();

    // Check that we don't try to copy over the cursor area, and
    // if that happens we need to treat it as changed so that we can
    // re-render it
    if (!ui.copied.intersect(renderedCursorRect).is_empty()) {
      ui.changed.assign_union(ui.copied.intersect(renderedCursorRect));
      ui.copied.assign_subtract(renderedCursorRect);
    }

    // Track where we've rendered the cursor
    damagedCursorRegion.assign_union(ui.changed.intersect(renderedCursorRect));
  }

  ui.copypassed = copypassed;
  if (!pending.is_empty())
    ui.copypassed.clear();

  // Do we need to send a full frame?
  if (Server::udpFullFrameFrequency && cp.supportsUdp) {
    if (udpFramesSinceFull >= (unsigned) Server::udpFullFrameFrequency) {
      udpFramesSinceFull = 0;
      ui.changed.assign_union(Region(Rect(0, 0, cp.width, cp.height)));
    }
  }

  // Return if there is nothing to send the client.
  const unsigned losslessThreshold = 80 + 2 * 1000 / Server::frameRate;

  if (ui.is_empty() && !writer()->needFakeUpdate() &&
      (!encodeManager.needsLosslessRefresh(req) ||
      msSince(&lastRealUpdate) < losslessThreshold))
    return;

  writeRTTPing();

  // FIXME: If continuous updates aren't used then the client might
  //        be slower than frameRate in its requests and we could
  //        afford a larger update size

  // FIXME: Bandwidth estimation without congestion control
  maxUpdateSize = congestion.getBandwidth() *
                  server->msToNextUpdate() / 1000;

  if (!ui.is_empty()) {
    encodeManager.writeUpdate(ui, server->getPixelBuffer(), cursor, maxUpdateSize);
    copypassed.clear();
    gettimeofday(&lastRealUpdate, NULL);
    losslessTimer.start(losslessThreshold);

    const unsigned ms = encodeManager.getEncodingTime();
    const unsigned limit = 1000 / rfb::Server::frameRate;
    if (ms >= limit) {
        bstats[BS_CPU_SLOW].push_back(lastRealUpdate);
        bstats_total[BS_CPU_SLOW]++;

        // If it was several frames' worth, add several so as to react faster
        int i = ms / limit;
        i--;
        for (; i > 0; i--) {
            bstats[BS_CPU_SLOW].push_back(lastRealUpdate);
            bstats_total[BS_CPU_SLOW]++;

            bstats[BS_FRAME].push_back(lastRealUpdate);
            bstats_total[BS_FRAME]++;
        }
    } else if (ms >= limit * 0.8f) {
        bstats[BS_CPU_CLOSE].push_back(lastRealUpdate);
        bstats_total[BS_CPU_CLOSE]++;
    }
  } else {
    encodeManager.writeLosslessRefresh(req, server->getPixelBuffer(),
                                       cursor, maxUpdateSize);
  }

  writeRTTPing();

  // The request might be for just part of the screen, so we cannot
  // just clear the entire update tracker.
  updates.subtract(req);

  requested.clear();

  if (Server::udpFullFrameFrequency && cp.supportsUdp)
    udpFramesSinceFull++;
}

void VNCSConnectionST::writeBinaryClipboard()
{
  if (msSince(&lastClipboardOp) < (unsigned) rfb::Server::DLP_ClipDelay) {
    vlog.info("DLP: client %s: refused to send binary clipboard, too soon",
              sock->getPeerAddress());
    return;
  }

  writer()->writeBinaryClipboard(binaryClipboard);

  gettimeofday(&lastClipboardOp, NULL);
}

void VNCSConnectionST::screenLayoutChange(rdr::U16 reason)
{
  if (!authenticated())
    return;

  cp.screenLayout = server->screenLayout;

  if (state() != RFBSTATE_NORMAL)
    return;

  writer()->writeExtendedDesktopSize(reason, 0, cp.width, cp.height,
                                     cp.screenLayout);
}

static const unsigned recentSecs = 10;

static void pruneStatList(std::list<struct timeval> &list, const struct timeval &now) {
  std::list<struct timeval>::iterator it;
  for (it = list.begin(); it != list.end(); ) {
    if ((*it).tv_sec + recentSecs < now.tv_sec)
      it = list.erase(it);
    else
      it++;
  }
}

void VNCSConnectionST::sendStats(const bool toClient) {
  char buf[1024];
  struct timeval now;

  // Prune too old stats from the recent lists
  gettimeofday(&now, NULL);

  pruneStatList(bstats[BS_CPU_CLOSE], now);
  pruneStatList(bstats[BS_CPU_SLOW], now);
  pruneStatList(bstats[BS_NET_SLOW], now);
  pruneStatList(bstats[BS_FRAME], now);

  const unsigned minuteframes = bstats[BS_FRAME].size();

  // Calculate stats
  float cpu_recent = bstats[BS_CPU_SLOW].size() + bstats[BS_CPU_CLOSE].size() * 0.2f;
  cpu_recent /= minuteframes;

  float cpu_total = bstats_total[BS_CPU_SLOW] + bstats_total[BS_CPU_CLOSE] * 0.2f;
  cpu_total /= bstats_total[BS_FRAME];

  float net_recent = bstats[BS_NET_SLOW].size();
  net_recent /= minuteframes;
  if (net_recent > 1)
    net_recent = 1;

  float net_total = bstats_total[BS_NET_SLOW];
  net_total /= bstats_total[BS_FRAME];
  if (net_total > 1)
    net_total = 1;

  #define ten(x) (10 - x * 10.0f)

  sprintf(buf, "[ %.1f, %.1f, %.1f, %.1f ]",
               ten(cpu_recent), ten(cpu_total),
               ten(net_recent), ten(net_total));

  #undef ten

  if (toClient) {
    vlog.info("Sending client stats:\n%s\n", buf);
    writer()->writeStats(buf, strlen(buf));
  } else if (server->apimessager) {
    server->apimessager->mainUpdateBottleneckStats(peerEndpoint.buf, buf);
  }
}

void VNCSConnectionST::handleFrameStats(rdr::U32 all, rdr::U32 render)
{
  if (server->apimessager) {
    const char *at = strrchr(peerEndpoint.buf, '@');
    if (!at)
      at = peerEndpoint.buf;
    else
      at++;

    server->apimessager->mainUpdateClientFrameStats(at, render, all,
                                                    congestion.getPingTime());
  }

  frameTracking = false;
}

// setCursor() is called whenever the cursor has changed shape or pixel format.
// If the client supports local cursor then it will arrange for the cursor to
// be sent to the client.

void VNCSConnectionST::setCursor()
{
  if (state() != RFBSTATE_NORMAL)
    return;

  // We need to blank out the client's cursor or there will be two
  if (needRenderedCursor()) {
    cp.setCursor(emptyCursor);
    clientHasCursor = false;
  } else {
    cp.setCursor(*server->cursor);
    clientHasCursor = true;
  }

  if (!writer()->writeSetVMwareCursor()) {
    if (!writer()->writeSetCursorWithAlpha()) {
      if (!writer()->writeSetCursor()) {
        if (!writer()->writeSetXCursor()) {
          // No client support
          return;
        }
      }
    }
  }
}

// setCursorPos() is called whenever the cursor has changed position by the
// server.  If the client supports being informed about these changes then it
// will arrange for the new cursor position to be sent to the client.

void VNCSConnectionST::setCursorPos()
{
  if (state() != RFBSTATE_NORMAL)
    return;

  if (cp.supportsCursorPosition) {
    cp.setCursorPos(server->cursorPos);
    writer()->writeCursorPos();
  }
}

void VNCSConnectionST::setDesktopName(const char *name)
{
  cp.setName(name);

  if (state() != RFBSTATE_NORMAL)
    return;

  if (!writer()->writeSetDesktopName()) {
    fprintf(stderr, "Client does not support desktop rename\n");
    return;
  }
}

void VNCSConnectionST::setLEDState(unsigned int ledstate)
{
  if (state() != RFBSTATE_NORMAL)
    return;

  cp.setLEDState(ledstate);

  writer()->writeLEDState();
}

void VNCSConnectionST::setSocketTimeouts()
{
  int timeoutms = rfb::Server::clientWaitTimeMillis;
  soonestTimeout(&timeoutms, secsToMillis(rfb::Server::idleTimeout));
  if (timeoutms == 0)
    timeoutms = -1;
  sock->inStream().setTimeout(timeoutms);
  sock->outStream().setTimeout(timeoutms);
}

char* VNCSConnectionST::getStartTime()
{
  char* result = ctime(&startTime);
  result[24] = '\0';
  return result; 
}

void VNCSConnectionST::setStatus(int status)
{
  switch (status) {
  case 0:
    accessRights = accessRights | AccessPtrEvents | AccessKeyEvents | AccessView;
    break;
  case 1:
    accessRights = (accessRights & ~(AccessPtrEvents | AccessKeyEvents)) | AccessView;
    break;
  case 2:
    accessRights = accessRights & ~(AccessPtrEvents | AccessKeyEvents | AccessView);
    break;
  }
  framebufferUpdateRequest(server->pb->getRect(), false);
}
int VNCSConnectionST::getStatus()
{
  if ((accessRights & (AccessPtrEvents | AccessKeyEvents | AccessView)) == 0x0007)
    return 0;
  if ((accessRights & (AccessPtrEvents | AccessKeyEvents | AccessView)) == 0x0001)
    return 1;
  if ((accessRights & (AccessPtrEvents | AccessKeyEvents | AccessView)) == 0x0000)
    return 2;
  return 4;
}

bool VNCSConnectionST::checkOwnerConn() const
{
  std::list<VNCSConnectionST*>::const_iterator it;

  for (it = server->clients.begin(); it != server->clients.end(); it++) {
    bool read, write, owner;
    if ((*it)->getPerms(read, write, owner) && owner)
      return true;
  }

  return false;
}

void VNCSConnectionST::udpUpgrade(const char *resp)
{
  if (resp[0] == 'H') {
    vlog.info("Client %s requested upgrade to udp, but WebUdp refused", sock->getPeerAddress());
  } else {
    vlog.info("Client %s requesting upgrade to udp", sock->getPeerAddress());
    upgradingToUdp = true;
  }
  writer()->writeUdpUpgrade(resp);
}

void VNCSConnectionST::udpDowngrade(const bool byServer)
{
  cp.supportsUdp = false;
  cp.useCopyRect = true;
  encodeManager.resetZlib();

  if (Server::DLP_WatermarkImage[0])
    cp.useCopyRect = false;

  vlog.info("Client %s downgrading from udp by %s", sock->getPeerAddress(),
            byServer ? "the server" : "its own request");
}

void VNCSConnectionST::subscribeUnixRelay(const char *name)
{
  bool read, write, owner;
  if (!getPerms(read, write, owner) || !write) {
    // Need write permissions to subscribe
    writer()->writeSubscribeUnixRelay(false, "No permissions");
    vlog.info("Client tried to subscribe to unix channel %s without permissions", name);
    return;
  }

  unsigned i;
  bool found = false;
  for (i = 0; i < MAX_UNIX_RELAYS; i++) {
    if (!strcmp(name, unixrelaynames[i])) {
      found = true;
      break;
    }
  }

  if (!found) {
    writer()->writeSubscribeUnixRelay(false, "No such unix channel");
    vlog.info("Client tried to subscribe to nonexistent unix channel %s", name);
    return;
  }

  writer()->writeSubscribeUnixRelay(true, "Ok");
  for (i = 0; i < MAX_UNIX_RELAYS; i++) {
    if (!unixRelaySubscriptions[i][0]) {
      strcpy(unixRelaySubscriptions[i], name);
      break;
    }
  }
}

void VNCSConnectionST::unixRelay(const char *name, const rdr::U8 *buf, const unsigned len)
{
  unsigned i;
  for (i = 0; i < MAX_UNIX_RELAYS; i++) {
    if (!strcmp(unixRelaySubscriptions[i], name)) {
      server->desktop->receivedUnixRelayData(name, buf, len);
      return;
    }
  }
}

void VNCSConnectionST::sendUnixRelayData(const char name[], const unsigned char *buf,
                                         const unsigned len)
{
  writer()->writeUnixRelay(name, buf, len);
}
