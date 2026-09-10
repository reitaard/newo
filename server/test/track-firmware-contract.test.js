import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

const read = (name) => readFile(new URL(`../../Newo/${name}`, import.meta.url), "utf8");

test("combined CSI classifies AP and peer independently and preserves NCSI metadata", async () => {
  const source = await read("newo_csi_measurement.cpp");
  assert.match(source, /bool ap=memcmp\(info->mac,apMac,6\)==0,peer=memcmp\(info->mac,peerMac,6\)==0/);
  assert.match(source, /uint16_t path=peer\?3:1/);
  assert.match(source, /if\(peer&&peerLastAcceptedUs/);
  assert.doesNotMatch(source, /if\(ap&&.*Gate/);
  assert.match(source, /NCSI_FLAG_FIRST_WORD_INVALID_REPORTED\|NCSI_FLAG_INVALID_PREFIX_SANITIZED/);
  assert.match(source, /memcpy\(s\.csi,info->buf,info->len\)/);
  assert.match(source, /driver_rx_timestamp_us=info->rx_ctrl.timestamp/);
  assert.match(source, /driver_rx_sequence=info->rx_seq/);
});

test("TRACK OFF releases local resources even when peer acknowledgement is absent", async () => {
  const source = await read("newo_tracking.cpp");
  const stop = source.slice(source.indexOf("bool NewoTracking::stop()"), source.indexOf("NewoTracking::Result"));
  assert.match(stop, /bool peerStopped=requestPeer\(false\)/);
  assert.match(stop, /esp_wifi_set_csi\(false\)/);
  assert.match(stop, /newoCollectorDiscoveryStop\(\)/);
  assert.match(stop, /endPeer\(\)/);
  assert.match(stop, /state_=State::OFF/);
  assert.doesNotMatch(stop, /if\(!peerStopped\)return false/);
});

test("tracking owns ESP-NOW only after successful init and releases only owned state", async () => {
  const source = await read("newo_tracking.cpp");
  assert.match(source, /if\(e!=ESP_OK\)return false;ownsEspNow=true/);
  assert.match(source, /if\(!ownsEspNow\)return;esp_now_unregister_recv_cb/);
});

test("track ACK contract includes command identity, failure, and peer state", async () => {
  const cloud = await read("newo_cloud.cpp");
  const server = await readFile(new URL("../src/index.js", import.meta.url), "utf8");
  assert.match(cloud, /doc\["command_epoch"\] = commandEpoch/);
  assert.match(cloud, /doc\["command_sequence"\] = commandSequence/);
  assert.match(cloud, /"queue_full"/);
  assert.match(cloud, /doc\["peer_state"\] = peerStatus/);
  assert.match(server, /message\.command_epoch !== pending\.fields\?\.command_epoch/);
  assert.match(server, /message\.command_sequence !== pending\.fields\?\.command_sequence/);
});

test("collector loss changes destination source, not TRACK state", async () => {
  const discovery = await read("newo_collector_discovery.cpp");
  const tracking = await read("newo_tracking.cpp");
  assert.match(discovery, /fresh\?"discovered":"configured"|source="discovered"/);
  assert.doesNotMatch(tracking, /collector.*state_=State::OFF/);
});
