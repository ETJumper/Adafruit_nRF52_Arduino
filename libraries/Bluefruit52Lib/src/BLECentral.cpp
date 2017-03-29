/**************************************************************************/
/*!
    @file     BLECentral.cpp
    @author   hathach

    @section LICENSE

    Software License Agreement (BSD License)

    Copyright (c) 2017, Adafruit Industries (adafruit.com)
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:
    1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
    2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.
    3. Neither the name of the copyright holders nor the
    names of its contributors may be used to endorse or promote products
    derived from this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ''AS IS'' AND ANY
    EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
    WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
    DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY
    DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
    (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
    ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
    SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
/**************************************************************************/

#include "bluefruit.h"

#define BLE_CENTRAL_TIMEOUT     3000

/**
 * Constructor
 */
BLECentral::BLECentral(void)
{
  _conn_hdl    = BLE_CONN_HANDLE_INVALID;

  _evt_sem     = NULL;
  _evt_buf     = NULL;
  _evt_bufsize = 0;

  _txpacket_sem = NULL;

  _svc_count = 0;
  for(uint8_t i=0; i<BLE_CENTRAL_MAX_SERVICE; i++) _svc_list[i] = NULL;

  _chars_count = 0;
  for(uint8_t i=0; i<BLE_CENTRAL_MAX_CHARS; i++) _chars_list[i] = NULL;

  _scan_cb     = NULL;
  _scan_param  = (ble_gap_scan_params_t) {
    .active      = 1,
    .selective   = 0,
    .p_whitelist = NULL,
    .interval    = 0x00A0,
    .window      = 0x0050,
    .timeout     = 0, // no timeout
  };

  _disc_hdl_range.start_handle = 1;
  _disc_hdl_range.end_handle   = 0xffff;

  _connect_cb     = NULL;
  _disconnect_cb = NULL;
}

void BLECentral::begin(void)
{
  _evt_sem = xSemaphoreCreateBinary();
}

bool BLECentral::_registerService(BLECentralService* svc)
{
  VERIFY( _svc_count < BLE_CENTRAL_MAX_SERVICE );
  _svc_list[ _svc_count++ ] = svc;

  return true;
}

bool BLECentral::_registerCharacteristic(BLECentralCharacteristic* chr)
{
  VERIFY( _chars_count < BLE_CENTRAL_MAX_CHARS );
  _chars_list[ _chars_count++ ] = chr;

  return true;
}

bool BLECentral::getTxPacket(uint32_t ms)
{
  VERIFY(_txpacket_sem != NULL);
  return xSemaphoreTake(_txpacket_sem, ms2tick(ms));
}

/*------------------------------------------------------------------*/
/* Scan  and Parser
 *------------------------------------------------------------------*/
void BLECentral::setScanCallback(scan_callback_t fp)
{
  _scan_cb = fp;
}

err_t BLECentral::startScanning(uint16_t timeout)
{
  _scan_param.timeout = timeout;
  VERIFY_STATUS( sd_ble_gap_scan_start(&_scan_param) );
  Bluefruit.startConnLed(); // start blinking
  return ERROR_NONE;
}

err_t BLECentral::stopScanning(void)
{
  Bluefruit.stopConnLed(); // stop blinking
  return sd_ble_gap_scan_stop();
}

uint8_t* BLECentral::extractScanData(uint8_t const* scandata, uint8_t scanlen, uint8_t type, uint8_t* result_len)
{
  *result_len = 0;

  // len (1+data), type, data
  while ( scanlen )
  {
    if ( scandata[1] == type )
    {
      *result_len = scandata[0]-1;
      return (uint8_t*) (scandata + 2);
    }
    else
    {
      scanlen  -= (scandata[0] + 1);
      scandata += (scandata[0] + 1);
    }
  }

  return NULL;
}

uint8_t* BLECentral::extractScanData(const ble_gap_evt_adv_report_t* report, uint8_t type, uint8_t* result_len)
{
  return extractScanData(report->data, report->dlen, type, result_len);
}

bool BLECentral::checkUuidInScan(const ble_gap_evt_adv_report_t* report, BLEUuid ble_uuid)
{
  const uint8_t* uuid;
  uint8_t uuid_len = ble_uuid.size();

  uint8_t type_arr[2];

  // Check both UUID16 more available and complete list
  if ( uuid_len == 16)
  {
    type_arr[0] = BLE_GAP_AD_TYPE_16BIT_SERVICE_UUID_MORE_AVAILABLE;
    type_arr[1] = BLE_GAP_AD_TYPE_16BIT_SERVICE_UUID_COMPLETE;

    uuid = (uint8_t*) &ble_uuid._uuid.uuid;
  }else
  {
    type_arr[0] = BLE_GAP_AD_TYPE_128BIT_SERVICE_UUID_MORE_AVAILABLE;
    type_arr[1] = BLE_GAP_AD_TYPE_128BIT_SERVICE_UUID_COMPLETE;

    uuid = ble_uuid._uuid128;
  }

  uuid_len /= 8; // convert uuid_len to number of bytes

  for (int i=0; i<2; i++)
  {
    uint8_t len = 0;
    uint8_t const* data = extractScanData(report, type_arr[i] , &len);

    while( len )
    {
      // found matched
      if ( !memcmp(data, uuid, uuid_len) )
      {
        return true;
      }else
      {
        data += uuid_len;
        len  -= uuid_len;
      }
    }
  }

  return false;
}

/*------------------------------------------------------------------*/
/*
 *------------------------------------------------------------------*/
err_t BLECentral::connect(const ble_gap_addr_t* peer_addr, uint16_t min_conn_interval, uint16_t max_conn_interval)
{
  ble_gap_conn_params_t gap_conn_params =
  {
      .min_conn_interval = min_conn_interval, // in 1.25ms unit
      .max_conn_interval = max_conn_interval, // in 1.25ms unit
      .slave_latency     = BLE_GAP_CONN_SLAVE_LATENCY,
      .conn_sup_timeout  = BLE_GAP_CONN_SUPERVISION_TIMEOUT_MS / 10 // in 10ms unit
  };

  return sd_ble_gap_connect(peer_addr, &_scan_param, &gap_conn_params);
}

err_t BLECentral::connect(const ble_gap_evt_adv_report_t* adv_report, uint16_t min_conn_interval, uint16_t max_conn_interval)
{
  return connect(&adv_report->peer_addr, min_conn_interval, max_conn_interval);
}

bool BLECentral::connected(void)
{
  return (_conn_hdl != BLE_CONN_HANDLE_INVALID);
}

uint16_t BLECentral::connHandle (void)
{
  return _conn_hdl;
}

void BLECentral::setConnectCallback( connect_callback_t fp)
{
  _connect_cb = fp;
}

void BLECentral::setDisconnectCallback( disconnect_callback_t fp)
{
  _disconnect_cb = fp;
}

/*------------------------------------------------------------------*/
/* DISCOVERY
 *------------------------------------------------------------------*/
bool BLECentral::discoverService(BLECentralService& svc, uint16_t start_handle)
{
  ble_gattc_evt_prim_srvc_disc_rsp_t disc_svc;

  _evt_buf     = &disc_svc;
  _evt_bufsize = sizeof(disc_svc);

  VERIFY_STATUS( sd_ble_gattc_primary_services_discover(_conn_hdl, start_handle, &svc.uuid._uuid), false );

  // wait for discovery event: timeout or has no data
  if ( !xSemaphoreTake(_evt_sem, BLE_CENTRAL_TIMEOUT) || (_evt_bufsize == 0) ) return false;

  // Check the discovered UUID with input one
  if ( (disc_svc.count == 1) && (svc.uuid == disc_svc.services[0].uuid) )
  {
    _disc_hdl_range = disc_svc.services[0].handle_range;
    LOG_LV1(Discover, "[SVC] Found 0x%04X, Handle start = %d, end = %d", disc_svc.services[0].uuid.uuid, _disc_hdl_range.start_handle, _disc_hdl_range.end_handle);

    _disc_hdl_range.start_handle++; // increase for characteristic discovery
    return true;
  }

  return false;
}

uint8_t BLECentral::discoverCharacteristic(BLECentralCharacteristic* chr[], uint8_t count)
{
  uint8_t found = 0;

  while( found < count )
  {
    ble_gattc_evt_char_disc_rsp_t disc_chr;

    _evt_buf     = &disc_chr;
    _evt_bufsize = sizeof(disc_chr);

    LOG_LV1(Discover, "[CHR] Handle start = %d, end = %d", _disc_hdl_range.start_handle, _disc_hdl_range.end_handle);

    VERIFY_STATUS( sd_ble_gattc_characteristics_discover(_conn_hdl, &_disc_hdl_range), found );

    // wait for discovery event: timeout or has no data
    // Assume only 1 characteristic discovered each
    if ( !xSemaphoreTake(_evt_sem, BLE_CENTRAL_TIMEOUT) || (_evt_bufsize == 0) || (disc_chr.count == 0) ) break;

    // increase handle range for next discovery
    _disc_hdl_range.start_handle = disc_chr.chars[0].handle_value + 1;

    // Look for matched uuid
    for (uint8_t i=0; i<count; i++)
    {
      if ( chr[i]->uuid == disc_chr.chars[0].uuid )
      {
        LOG_LV1(Discover, "[CHR] Found 0x%04X, handle = %d", disc_chr.chars[0].uuid.uuid,  disc_chr.chars[0].handle_value);

        chr[i]->_chr = disc_chr.chars[0];

        // Discovery All descriptors as well
        chr[i]->discoverDescriptor();

        found++;

        break;
      }
    }
  }

  return found;
}

uint16_t BLECentral::_discoverDescriptor(ble_gattc_evt_desc_disc_rsp_t* disc_desc, uint16_t max_count)
{
  _evt_buf     = disc_desc;
  _evt_bufsize = sizeof(ble_gattc_evt_desc_disc_rsp_t) + (max_count-1)*sizeof(ble_gattc_desc_t);

  LOG_LV1(Discover, "[DESC] Handle start = %d, end = %d", _disc_hdl_range.start_handle, _disc_hdl_range.end_handle);

  uint16_t result = 0;
  VERIFY_STATUS( sd_ble_gattc_descriptors_discover(_conn_hdl, &_disc_hdl_range), 0 );

  // wait for discovery event: timeout or has no data
  if ( !xSemaphoreTake(_evt_sem, BLE_CENTRAL_TIMEOUT) || (_evt_bufsize == 0) ) return 0;

  result = min16(disc_desc->count, max_count);
  if (result)
  {
    for(uint16_t i=0; i<result; i++)
    {
      LOG_LV1(Discover, "[DESC] Descriptor %d: uuid = 0x%04X, handle = %d", i, disc_desc->descs[i].uuid.uuid, disc_desc->descs[i].handle);
    }

    // increase handle range for next discovery
    // should be +1 more, but that will cause missing on the next Characteristic !!!!!
    // Reason is descriptor also include BLE_UUID_CHARACTERISTIC 0x2803 (Char declaration) in the result
    _disc_hdl_range.start_handle = disc_desc->descs[result-1].handle;
  }

  return result;
}

/**
 * Event is forwarded from Bluefruit Poll() method
 * @param event
 */
void BLECentral::_event_handler(ble_evt_t* evt)
{
  // conn handle has fixed offset regardless of event type
  const uint16_t evt_conn_hdl = evt->evt.common_evt.conn_handle;

  /* Only handle Central events with matched connection handle
   * or a few special one
   * - Connected event
   * - Advertising Report
   * - Advertising timeout (could be connected and advertising at the same time)
   */
  if ( evt_conn_hdl       == _conn_hdl              ||
       evt->header.evt_id == BLE_GAP_EVT_CONNECTED  ||
       evt->header.evt_id == BLE_GAP_EVT_ADV_REPORT ||
       evt->header.evt_id == BLE_GAP_EVT_TIMEOUT )
  {
    switch ( evt->header.evt_id  )
    {
      case BLE_GAP_EVT_ADV_REPORT:
      {
        ble_gap_evt_adv_report_t* adv_report = &evt->evt.gap_evt.params.adv_report;
        if (_scan_cb) _scan_cb(adv_report);
      }
      break;

      case BLE_GAP_EVT_CONNECTED:
      {
        ble_gap_evt_connected_t* para = &evt->evt.gap_evt.params.connected;

        if (para->role == BLE_GAP_ROLE_CENTRAL)
        {
          Bluefruit.stopConnLed();
          if (Bluefruit._led_conn) ledOn(LED_BLUE);

          // TODO multiple connections
          _conn_hdl = evt->evt.gap_evt.conn_handle;

          // Init transmission buffer for notification. TODO multiple connections
          uint8_t txbuf_max;
          (void) sd_ble_tx_packet_count_get(_conn_hdl, &txbuf_max);
          _txpacket_sem = xSemaphoreCreateCounting(txbuf_max, txbuf_max);

          if ( _connect_cb ) _connect_cb();
        }
      }
      break;

      case BLE_GAP_EVT_DISCONNECTED:
        if (Bluefruit._led_conn)  ledOff(LED_BLUE);

        _conn_hdl = BLE_CONN_HANDLE_INVALID;

        // TODO multiple connections
        vSemaphoreDelete(_txpacket_sem);
        _txpacket_sem = NULL;

        // disconnect all registered services
        for(uint8_t i=0; i<_svc_count; i++) _svc_list[i]->disconnect();

        if ( _disconnect_cb ) _disconnect_cb(evt->evt.gap_evt.params.disconnected.reason);

        startScanning();
      break;

      case BLE_EVT_TX_COMPLETE:
        if ( _txpacket_sem )
        {
          for(uint8_t i=0; i<evt->evt.common_evt.params.tx_complete.count; i++)
          {
            xSemaphoreGive(_txpacket_sem);
          }
        }
      break;

      case BLE_GAP_EVT_TIMEOUT:
        if (evt->evt.gap_evt.params.timeout.src == BLE_GAP_TIMEOUT_SRC_SCAN)
        {
          // TODO Advance Scanning
          // Restart Scanning
          startScanning();
        }
      break;

      /*------------------------------------------------------------------*/
      /* DISCOVERY
       *------------------------------------------------------------------*/
      case BLE_GATTC_EVT_PRIM_SRVC_DISC_RSP:
      {
        ble_gattc_evt_t* gattc = &evt->evt.gattc_evt;
        ble_gattc_evt_prim_srvc_disc_rsp_t* svc_rsp = &gattc->params.prim_srvc_disc_rsp;

        if ( _conn_hdl == gattc->conn_handle )
        {
          LOG_LV1(Discover, "[SVC] Service Count: %d", svc_rsp->count);

          if (gattc->gatt_status == BLE_GATT_STATUS_SUCCESS && svc_rsp->count && _evt_buf)
          {
            // Only support 1 service

            _evt_bufsize = min16(_evt_bufsize, sizeof(ble_gattc_evt_prim_srvc_disc_rsp_t));
            memcpy(_evt_buf, svc_rsp, _evt_bufsize);
          }else
          {
            _evt_bufsize = 0; // no data
          }

          xSemaphoreGive(_evt_sem);
        }
      }
      break;

      case BLE_GATTC_EVT_CHAR_DISC_RSP:
      {
        ble_gattc_evt_t* gattc = &evt->evt.gattc_evt;
        ble_gattc_evt_char_disc_rsp_t* chr_rsp = &gattc->params.char_disc_rsp;

        if ( _conn_hdl == gattc->conn_handle )
        {
          LOG_LV1(Discover, "[CHR] Characteristic Count: %d", chr_rsp->count);
          if ( (gattc->gatt_status == BLE_GATT_STATUS_SUCCESS) && chr_rsp->count && _evt_buf )
          {
            // TODO support only 1 discovered char now
            _evt_bufsize = min16(_evt_bufsize, sizeof(ble_gattc_evt_char_disc_rsp_t));

            memcpy(_evt_buf, chr_rsp, _evt_bufsize);
          }else
          {
            _evt_bufsize = 0; // no data
          }
        }

        xSemaphoreGive(_evt_sem);
      }
      break;

      case BLE_GATTC_EVT_DESC_DISC_RSP:
      {
        ble_gattc_evt_t* gattc = &evt->evt.gattc_evt;
        ble_gattc_evt_desc_disc_rsp_t* desc_rsp = &gattc->params.desc_disc_rsp;

        if ( _conn_hdl == gattc->conn_handle )
        {
          LOG_LV1(Discover, "[DESC] Descriptor Count: %d", desc_rsp->count);

          if ( (gattc->gatt_status == BLE_GATT_STATUS_SUCCESS) && desc_rsp->count && _evt_buf )
          {
            // Copy up to bufsize
            uint16_t len = sizeof(ble_gattc_evt_desc_disc_rsp_t) + (desc_rsp->count-1)*sizeof(ble_gattc_desc_t);
            _evt_bufsize = min16(_evt_bufsize, len);

            memcpy(_evt_buf, desc_rsp, _evt_bufsize);
          }else
          {
            _evt_bufsize = 0; // no data
          }
        }

        xSemaphoreGive(_evt_sem);
      }
      break;

      default: break;
    }
  }

  // GATT characteristics event handler only if Connection Handle matches
  if ( evt_conn_hdl == _conn_hdl)
  {
    for(int i=0; i<_chars_count; i++)
    {
      bool matched = false;

      switch(evt->header.evt_id)
      {
        case BLE_GATTC_EVT_HVX:
        case BLE_GATTC_EVT_WRITE_RSP:
        case BLE_GATTC_EVT_READ_RSP:
          // write & read response's handle has same offset.
          matched = (_chars_list[i]->_chr.handle_value == evt->evt.gattc_evt.params.write_rsp.handle);
        break;

        default: break;
      }

      // invoke charactersistic handler if matched
      if ( matched ) _chars_list[i]->_eventHandler(evt);
    }
  }
}
