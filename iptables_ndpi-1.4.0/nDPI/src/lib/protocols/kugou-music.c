#include "ndpi_protocols.h"

#ifdef NDPI_PROTOCOL_KUGOUMUSIC

#undef _D
#define _D(...) NDPI_LOG(NDPI_PROTOCOL_KUGOUMUSIC, ndpi, NDPI_LOG_DEBUG, __VA_ARGS__)

static u_int32_t kugou_hash(u_int8_t const *data, int len)
{
    u_int32_t ret = 0;
    u_int8_t *p = (u_int8_t*)&ret;
    int i;
    if (len < 16) return 0;
    for (i = 0; i < 4; i++) {
        p[0] ^= data[i];
        p[1] ^= data[i+1];
        p[2] ^= data[i+2];
        p[3] ^= data[i+3];
    }
    return ret;
}
/**
 * search KuGouMusic over udp protocol
 */
static void search_udp(struct ndpi_detection_module_struct *ndpi, struct ndpi_flow_struct *flow)
{
    struct ndpi_packet_struct *pkt = &flow->packet;
    u_int8_t const *payload = pkt->payload;
    int paylen = pkt->payload_packet_len;
    const int hash_offset = 5;
    const int seq_offset = 26;
    const int suffix_offset = 30;
    _D("KuGouMusic: In ndpi_search_kugou_music, called search_udp.\n");
    _D("KuGouMusic: stage: %d\n", flow->kugou_music_stage);
    _D("KuGouMusic: len: %d, seq: %02x, hash: %02x, suf: %02x\n",
            paylen, payload[seq_offset], payload[hash_offset], payload[suffix_offset]);
    _D("KuGouMusic: flow->hash: %08x\n", flow->kugou_music_hash);
    switch (flow->kugou_music_stage) {
    case 0:
        if (paylen >= 30 && 0x65 == payload[0])
            flow->kugou_music_stage = 1;
        break;

    case 1:
        if (paylen >= 700 && 0x32 == payload[0]
                && (0x00 == payload[suffix_offset] && 0x04 == payload[suffix_offset+1])) {
            flow->kugou_music_udp_seq = payload[seq_offset];
            flow->kugou_music_hash = kugou_hash(payload+hash_offset, paylen-hash_offset);
            flow->kugou_music_stage = 2;
        }
        break;

    case 2:
        if (paylen >= 700 && 0x32 == payload[0]
                && (0x00 == payload[suffix_offset] && 0x04 == payload[suffix_offset+1])) {
            _D("KuGouMusic: pkt->hash: %08x\n", kugou_hash(payload+hash_offset, paylen-hash_offset));
            //if (flow->kugou_music_udp_seq+1 == payload[seq_offset]
            if (flow->kugou_music_hash == kugou_hash(payload+hash_offset, paylen-hash_offset)) {
                _D("KuGouMusic: Found KuGouMusic(udp).\n");
                ndpi_int_add_connection(ndpi, flow, NDPI_PROTOCOL_KUGOUMUSIC, NDPI_REAL_PROTOCOL);
            } else {
                _D("KuGouMusic: exclude KuGouMusic(udp).\n");
                NDPI_ADD_PROTOCOL_TO_BITMASK(flow->excluded_protocol_bitmask, NDPI_PROTOCOL_KUGOUMUSIC);
            }
        }
        break;
    }
    _D("KuGouMusic: end search_udp(): stage: %d\n", flow->kugou_music_stage);
}

extern void ndpi_search_kugou_music(struct ndpi_detection_module_struct *ndpi, struct ndpi_flow_struct *flow)
{
    _D("KuGouMusic: Called ndpi_search_kugou_music.\n");
    _D("KuGouMusic: type: %d\n", flow->kugou_music_type);
    if (flow->kugou_music_type != 0 && flow->kugou_music_type != 2)
        return;

    if (flow->packet.udp) {
        flow->kugou_music_type = 2;
        search_udp(ndpi, flow);
    }
}

#endif /* NDPI_PROTOCOL_KUGOUMUSIC */
