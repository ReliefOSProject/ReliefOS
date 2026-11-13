#define N_(s) (s)

/* ReliefPlayer labels returned by the decoder error table. */
static const char *const reliefplayer_static_labels[] = {
    N_("ReliefPlayer"),
    N_("Could not read the media file"),
    N_("Invalid or truncated media file"),
    N_("Unsupported media format or codec"),
    N_("Media exceeds the supported size or resolution"),
    N_("Not enough memory"),
    N_("Audio device is unavailable"),
    N_("Audio device is busy"),
    N_("Media playback failed"),
    N_("Playback process stopped unexpectedly"),
};

/* Strings supplied by shared code and dynamic application labels. */
static const char *const reliefplayer_indirect_messages[] = {
    N_("Cancelled"),
    N_("Ready"),
    N_("failed"),
};
