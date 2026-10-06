/* Exercise the production devfs parser against the production audio registry. */
#include <assert.h>
#include <stdio.h>
#include "audio_fixture.h"
#include "audio_fake_card.h"
#include "../../kernel/reliefnt/kernel/reliefnt/audio/core.c"
#include "../../kernel/reliefnt/kernel/reliefnt/audio/device.c"
#include "../../kernel/reliefnt/drivers/bootstrap/storage/storage_audio.c"

static int sparse_caps(void *opaque, uint32_t device, enum audio_direction direction,
                       struct audio_caps *caps)
{
    if (device != 2 || direction != AUDIO_PLAYBACK) return -ENODEV;
    return audio_test_caps(opaque, device, direction, caps);
}

int main(void)
{
    struct audio_test_card card;
    struct audio_card_ops ops = audio_test_ops;
    ops.pcm_caps = sparse_caps;
    memset(&card, 0, sizeof(card));
    struct audio_card_identity identity = {.id = "sparse", .name = "sparse-card"};
    assert(!audio_register_card_owned(&identity, &ops, &card, 0x7a, &card.id));
    struct storage_node node;
    assert(!storage_audio_control_node("controlC0", &node));
    assert(!storage_audio_pcm_node("pcmC0D2p", &node));
    assert(AUDIO_DEVICE_CARD(node.volume_id) == card.id);
    assert(AUDIO_DEVICE_INDEX(node.volume_id) == 2);
    assert(storage_audio_pcm_node("pcmC0D0p", &node) == -ENOENT);
    assert(storage_audio_pcm_node("pcmC0D2c", &node) == -ENOENT);
    assert(storage_audio_pcm_node("pcmC0D8p", &node) == -ENOENT);
    assert(storage_audio_pcm_node("pcmC0D2pjunk", &node) == -ENOENT);
    assert(storage_audio_control_node("controlC42949672960", &node) == -ENOENT);
    uint64_t cursor = 0;
    struct reliefos_dir_entry entry;
    assert(storage_audio_snd_readdir(&cursor, &entry) == 1);
    assert(!strcmp(entry.name, "timer") && entry.type == RELIEFOS_FS_TYPE_DEVICE);
    assert(storage_audio_snd_readdir(&cursor, &entry) == 1);
    assert(!strcmp(entry.name, "controlC0") && entry.type == RELIEFOS_FS_TYPE_DEVICE);
    assert(storage_audio_snd_readdir(&cursor, &entry) == 1);
    assert(!strcmp(entry.name, "pcmC0D2p"));
    assert(!storage_audio_snd_readdir(&cursor, &entry));
    cursor = UINT64_MAX;
    assert(!storage_audio_snd_readdir(&cursor, &entry));
    assert(!audio_unregister_card(card.id, 0));
    assert(storage_audio_pcm_node("pcmC0D2p", &node) == -ENOENT);
    puts("PASS production devfs sparse PCM capabilities and node identities");
}
