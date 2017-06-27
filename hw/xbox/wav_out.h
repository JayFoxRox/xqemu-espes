// This exists at least 2 times in QEMU, but none of them is exported cleanly..

typedef struct {
    FILE *f;
    int bytes;
    char *path;
    int freq;
    int bits;
    int nchannels;
} WAVOutState;

/* VICE code: Store number as little endian. */
static void le_store (uint8_t *buf, uint32_t val, int len)
{
    int i;
    for (i = 0; i < len; i++) {
        buf[i] = (uint8_t) (val & 0xff);
        val >>= 8;
    }
}

static void wav_out_update (WAVOutState *wav)
{
    uint8_t rlen[4];
    uint8_t dlen[4];
    uint32_t datalen = wav->bytes;
    uint32_t rifflen = datalen + 36;

    le_store (rlen, rifflen, 4);
    le_store (dlen, datalen, 4);

    long pos = ftell(wav->f);

    if (fseek (wav->f, 4, SEEK_SET)) {
        printf ("wav_out_update: rlen fseek failed\nReason: %s\n",
                        strerror (errno));
    }
    if (fwrite (rlen, 4, 1, wav->f) != 1) {
        printf ("wav_out_update: rlen fwrite failed\nReason %s\n",
                        strerror (errno));
    }
    if (fseek (wav->f, 32, SEEK_CUR)) {
        printf ("wav_out_update: dlen fseek failed\nReason %s\n",
                        strerror (errno));
    }
    if (fwrite (dlen, 1, 4, wav->f) != 4) {
        printf ("wav_out_update: dlen fwrite failed\nReason %s\n",
                        strerror (errno));
    }

    if (fseek (wav->f, pos, SEEK_SET)) {
        printf ("wav_out_update: rlen fseek failed\nReason: %s\n",
                        strerror (errno));
    }
}

static void wav_out_stop(WAVOutState *wav) {
    if (fclose (wav->f)) {
        fprintf (stderr, "wav_out_stop: fclose failed: %s",
                 strerror (errno));
    }

    g_free (wav->path);
}

static void wav_out_write (WAVOutState *wav, void *buf, int size)
{
    if (fwrite (buf, size, 1, wav->f) != 1) {
        printf ("wav_out_capture: fwrite error\nReason: %s",
                strerror (errno));
    }
    wav->bytes += size;
}

int wav_out_init (WAVOutState *wav, const char *path, int freq,
                  int bits, int nchannels)
{
    uint8_t hdr[] = {
        0x52, 0x49, 0x46, 0x46, 0x00, 0x00, 0x00, 0x00, 0x57, 0x41, 0x56,
        0x45, 0x66, 0x6d, 0x74, 0x20, 0x10, 0x00, 0x00, 0x00, 0x01, 0x00,
        0x02, 0x00, 0x44, 0xac, 0x00, 0x00, 0x10, 0xb1, 0x02, 0x00, 0x04,
        0x00, 0x10, 0x00, 0x64, 0x61, 0x74, 0x61, 0x00, 0x00, 0x00, 0x00
    };

    if (bits % 8 != 0) {
        printf ("incorrect bit count %d, must be multiple of 8\n", bits);
        return -1;
    }

    if (nchannels < 1) {
        printf ("incorrect channel count %d, must be greater than 1\n",
                        nchannels);
        return -1;
    }

    hdr[34] = bits;
    unsigned int m = nchannels * bits / 8;

    le_store (hdr + 22, nchannels, 2);
    le_store (hdr + 24, freq, 4);
    le_store (hdr + 28, freq * m, 4);
    le_store (hdr + 32, m, 2);

    wav->f = fopen (path, "wb");
    if (!wav->f) {
        printf ("Failed to open wave file `%s'\nReason: %s\n",
                        path, strerror (errno));
        return -1;
    }

    wav->path = g_strdup (path);
    wav->bits = bits;
    wav->nchannels = nchannels;
    wav->freq = freq;

    if (fwrite (hdr, sizeof (hdr), 1, wav->f) != 1) {
        printf ("Failed to write header\nReason: %s\n",
                        strerror (errno));
        goto error_free;
    }

    return 0;

error_free:
    g_free (wav->path);
    if (fclose (wav->f)) {
        printf ("Failed to close wave file\nReason: %s\n",
                        strerror (errno));
    }
    return -1;
}
