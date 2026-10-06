/* A1 host-side layout probe for the Linux v6.14 audio UAPI subset. */
#include <stddef.h>
#include <stdio.h>

#include <linux/soundcard.h>
#include <sound/asound.h>
#include <sound/tlv.h>

_Static_assert(sizeof(struct snd_pcm_hw_params) == 608, "snd_pcm_hw_params ABI");
_Static_assert(sizeof(struct snd_pcm_sw_params) == 136, "snd_pcm_sw_params ABI");
_Static_assert(sizeof(struct snd_pcm_status) == 152, "snd_pcm_status ABI");
_Static_assert(sizeof(struct snd_pcm_sync_ptr) == 136, "snd_pcm_sync_ptr ABI");
_Static_assert(sizeof(struct snd_xferi) == 24, "snd_xferi ABI");
_Static_assert(sizeof(struct snd_ctl_elem_info) == 272, "snd_ctl_elem_info ABI");
_Static_assert(sizeof(struct snd_ctl_elem_value) == 1224, "snd_ctl_elem_value ABI");
_Static_assert(sizeof(struct snd_ctl_elem_list) == 80, "snd_ctl_elem_list ABI");
_Static_assert(sizeof(struct snd_ctl_event) == 72, "snd_ctl_event ABI");

_Static_assert(SNDRV_PCM_IOCTL_HW_PARAMS == 0xc2604111UL, "HW_PARAMS ioctl");
_Static_assert(SNDRV_PCM_IOCTL_SW_PARAMS == 0xc0884113UL, "SW_PARAMS ioctl");
_Static_assert(SNDRV_CTL_IOCTL_ELEM_READ == 0xc4c85512UL, "ELEM_READ ioctl");
_Static_assert(SNDRV_PCM_IOCTL_WRITEI_FRAMES == 0x40184150UL, "WRITEI ioctl");

static void print_layout(void)
{
#define SIZE(type) printf("sizeof.%s=%zu\n", #type, sizeof(type))
#define ALIGN(type) printf("alignof.%s=%zu\n", #type, _Alignof(type))
#define OFFSET(type, member) printf("offsetof.%s.%s=%zu\n", #type, #member, offsetof(type, member))
#define VALUE(name) printf("value.%s=%lu\n", #name, (unsigned long)(name))

    SIZE(struct snd_pcm_hw_params);
    SIZE(struct snd_pcm_sw_params);
    SIZE(struct snd_pcm_status);
    SIZE(struct snd_pcm_sync_ptr);
    SIZE(struct __snd_pcm_mmap_status64);
    SIZE(struct __snd_pcm_mmap_control64);
    SIZE(struct __snd_pcm_sync_ptr64);
    SIZE(struct __snd_timespec64);
    SIZE(struct snd_xferi);
    SIZE(struct snd_ctl_elem_info);
    SIZE(struct snd_ctl_elem_value);
    SIZE(struct snd_ctl_elem_list);
    SIZE(struct snd_ctl_event);

    ALIGN(struct snd_pcm_hw_params);
    ALIGN(struct snd_pcm_status);
    ALIGN(struct snd_pcm_sync_ptr);
    ALIGN(struct __snd_pcm_mmap_status64);
    ALIGN(struct __snd_pcm_sync_ptr64);
    ALIGN(struct snd_xferi);
    ALIGN(struct snd_ctl_elem_info);
    ALIGN(struct snd_ctl_elem_value);
    ALIGN(struct snd_ctl_event);

    OFFSET(struct snd_pcm_hw_params, flags);
    OFFSET(struct snd_pcm_hw_params, fifo_size);
    OFFSET(struct snd_pcm_hw_params, sync);
    OFFSET(struct snd_pcm_sw_params, avail_min);
    OFFSET(struct snd_pcm_sw_params, boundary);
    OFFSET(struct snd_pcm_sw_params, reserved);
    OFFSET(struct snd_pcm_status, trigger_tstamp);
    OFFSET(struct snd_pcm_status, audio_tstamp);
    OFFSET(struct snd_pcm_status, reserved);
    OFFSET(struct snd_pcm_sync_ptr, flags);
    OFFSET(struct snd_pcm_sync_ptr, s);
    OFFSET(struct snd_pcm_sync_ptr, c);
    OFFSET(struct __snd_pcm_mmap_status64, hw_ptr);
    OFFSET(struct __snd_pcm_mmap_status64, tstamp);
    OFFSET(struct __snd_pcm_mmap_status64, audio_tstamp);
    OFFSET(struct __snd_pcm_mmap_control64, appl_ptr);
    OFFSET(struct __snd_pcm_mmap_control64, avail_min);
    OFFSET(struct __snd_pcm_sync_ptr64, s);
    OFFSET(struct __snd_pcm_sync_ptr64, c);
    OFFSET(struct snd_xferi, result);
    OFFSET(struct snd_xferi, buf);
    OFFSET(struct snd_xferi, frames);
    OFFSET(struct snd_ctl_elem_info, id);
    OFFSET(struct snd_ctl_elem_info, type);
    OFFSET(struct snd_ctl_elem_info, value);
    OFFSET(struct snd_ctl_elem_info, reserved);
    OFFSET(struct snd_ctl_elem_value, id);
    OFFSET(struct snd_ctl_elem_value, value);
    OFFSET(struct snd_ctl_elem_value, reserved);
    OFFSET(struct snd_ctl_elem_list, pids);
    OFFSET(struct snd_ctl_elem_list, reserved);
    OFFSET(struct snd_ctl_event, type);
    OFFSET(struct snd_ctl_event, data);

    VALUE(SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    VALUE(SNDRV_PCM_FORMAT_S16_LE);
    VALUE(SNDRV_PCM_STATE_OPEN);
    VALUE(SNDRV_CTL_ELEM_TYPE_BOOLEAN);
    VALUE(SNDRV_CTL_TLVT_DB_SCALE);
    VALUE(SOUND_VERSION);
    VALUE(AFMT_S16_LE);
    VALUE(SNDCTL_DSP_RESET);
    VALUE(SNDRV_PCM_IOCTL_HW_PARAMS);
    VALUE(SNDRV_PCM_IOCTL_SW_PARAMS);
    VALUE(SNDRV_CTL_IOCTL_ELEM_READ);
    VALUE(SNDRV_PCM_IOCTL_WRITEI_FRAMES);
}

#ifndef AUDIO_ABI_NO_MAIN
int main(void)
{
    print_layout();
    return 0;
}
#endif
