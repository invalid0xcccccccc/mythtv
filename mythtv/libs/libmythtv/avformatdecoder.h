
#ifndef AVFORMATDECODER_H_
#define AVFORMATDECODER_H_

#include <cstdint>

#include <QString>
#include <QMap>
#include <QList>

#include "programinfo.h"
#include "format.h"
#include "decoderbase.h"
#include "privatedecoder.h"
#include "audiooutputsettings.h"
#include "audiooutpututil.h"
#include "spdifencoder.h"
#include "vbilut.h"
#include "H264Parser.h"
#include "videodisplayprofile.h"
#include "mythplayer.h"

extern "C" {
#include "mythframe.h"
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
}

#include "avfringbuffer.h"

class TeletextDecoder;
class CC608Decoder;
class CC708Decoder;
class SubtitleReader;
class InteractiveTV;
class ProgramInfo;
class MythSqlDatabase;

struct SwsContext;

extern "C" void HandleStreamChange(void*);
extern "C" void HandleDVDStreamChange(void*);
extern "C" void HandleBDStreamChange(void*);

class AudioInfo
{
  public:
    AudioInfo() :
        codec_id(AV_CODEC_ID_NONE), format(FORMAT_NONE), sample_size(-2),
        sample_rate(-1), channels(-1), codec_profile(0),
        do_passthru(false), original_channels(-1)
    {;}

    AudioInfo(AVCodecID id, AudioFormat fmt, int sr, int ch, bool passthru,
              int original_ch, int profile = 0) :
        codec_id(id), format(fmt),
        sample_size(ch * AudioOutputSettings::SampleSize(fmt)),
            sample_rate(sr), channels(ch), codec_profile(profile),
            do_passthru(passthru), original_channels(original_ch)
    {
    }

    AVCodecID codec_id;
    AudioFormat format;
    int sample_size, sample_rate, channels, codec_profile;
    bool do_passthru;
    int original_channels;
    bool operator==(const AudioInfo &o) const
    {
        return (codec_id==o.codec_id        && channels==o.channels       &&
                sample_size==o.sample_size  && sample_rate==o.sample_rate &&
                format==o.format            && do_passthru==o.do_passthru &&
                original_channels==o.original_channels &&
                codec_profile == o.codec_profile);
    }
    QString toString() const
    {
        return QString("id(%1) %2Hz %3ch %4bps %5 (profile %6)")
            .arg(ff_codec_id_string(codec_id),4).arg(sample_rate,6)
            .arg(channels,2).arg(AudioOutputSettings::FormatToBits(format),2)
            .arg((do_passthru) ? "pt":"",3).arg(codec_profile);
    }
};

/// A decoder for video files.

/// The AvFormatDecoder is used to decode non-NuppleVideo files.
/// It's used as a decoder of last resort after trying the NuppelDecoder
class AvFormatDecoder : public DecoderBase
{
    friend void HandleStreamChange(void*);
    friend void HandleDVDStreamChange(void*);
    friend void HandleBDStreamChange(void*);
  public:
    static void GetDecoders(render_opts &opts);
    AvFormatDecoder(MythPlayer *parent, const ProgramInfo &pginfo,
                    PlayerFlags flags);
    virtual ~AvFormatDecoder();

    void SetEof(bool eof) override; // DecoderBase

    void CloseCodecs();
    void CloseContext();
    void Reset(bool reset_video_data, bool seek_reset,
               bool reset_file) override; // DecoderBase

    /// Perform an av_probe_input_format on the passed data to see if we
    /// can decode it with this class.
    static bool CanHandle(char testbuf[kDecoderProbeBufferSize], 
                          const QString &filename,
                          int testbufsize = kDecoderProbeBufferSize);

    /// Open our file and set up or audio and video parameters.
    int OpenFile(RingBuffer *rbuffer, bool novideo, 
                 char testbuf[kDecoderProbeBufferSize],
                 int testbufsize = kDecoderProbeBufferSize) override; // DecoderBase

    bool GetFrame(DecodeType) override; // DecoderBase

    bool IsLastFrameKey(void) const override { return false; } // DecoderBase

    bool IsCodecMPEG(void) const override // DecoderBase
        { return codec_is_mpeg; }

    /// This is a No-op for this class.
    void WriteStoredData(RingBuffer *rb, bool storevid,
                         long timecodeOffset) override // DecoderBase
        { (void)rb; (void)storevid; (void)timecodeOffset;}

    /// This is a No-op for this class.
    void SetRawAudioState(bool state) override { (void)state; } // DecoderBase

    /// This is a No-op for this class.
    bool GetRawAudioState(void) const override { return false; } // DecoderBase

    /// This is a No-op for this class.
    void SetRawVideoState(bool state) override { (void)state; } // DecoderBase

    /// This is a No-op for this class.
    bool GetRawVideoState(void) const override { return false; } // DecoderBase

    /// This is a No-op for this class.
    long UpdateStoredFrameNum(long frame) override { (void)frame; return 0;} // DecoderBase

    QString      GetCodecDecoderName(void) const override; // DecoderBase
    QString      GetRawEncodingType(void) override; // DecoderBase
    MythCodecID  GetVideoCodecID(void) const override { return video_codec_id; } // DecoderBase
    void        *GetVideoCodecPrivate(void) override; // DecoderBase

    void SetDisablePassThrough(bool disable) override; // DecoderBase
    void ForceSetupAudioStream(void) override; // DecoderBase
    void AddTextData(unsigned char *buf, int len, int64_t timecode, char type);

    QString GetTrackDesc(uint type, uint trackNo) const override; // DecoderBase
    int SetTrack(uint type, int trackNo) override; // DecoderBase

    int ScanStreams(bool novideo);
    int FindStreamInfo(void);

    int  GetNumChapters() override; // DecoderBase
    void GetChapterTimes(QList<long long> &times) override; // DecoderBase
    int  GetCurrentChapter(long long framesPlayed) override; // DecoderBase
    long long GetChapter(int chapter) override; // DecoderBase
    bool DoRewind(long long desiredFrame, bool doflush = true) override; // DecoderBase
    bool DoFastForward(long long desiredFrame, bool doflush = true) override; // DecoderBase
    void SetIdrOnlyKeyframes(bool value) override // DecoderBase
        { m_h264_parser->use_I_forKeyframes(!value); }

    int64_t NormalizeVideoTimecode(int64_t timecode) override; // DecoderBase
    virtual int64_t NormalizeVideoTimecode(AVStream *st, int64_t timecode);

    int  GetTeletextDecoderType(void) const override; // DecoderBase

    QString GetXDS(const QString&) const override; // DecoderBase
    QByteArray GetSubHeader(uint trackNo) const override; // DecoderBase
    void GetAttachmentData(uint trackNo, QByteArray &filename,
                           QByteArray &data) override; // DecoderBase

    // MHEG stuff
    bool SetAudioByComponentTag(int tag) override; // DecoderBase
    bool SetVideoByComponentTag(int tag) override; // DecoderBase

    // Stream language info
    virtual int GetTeletextLanguage(uint lang_idx) const;
    virtual int GetSubtitleLanguage(uint subtitle_index, uint stream_index);
    virtual int GetCaptionLanguage(TrackTypes trackType, int service_num);
    virtual int GetAudioLanguage(uint audio_index, uint stream_index);
    virtual AudioTrackType GetAudioTrackType(uint stream_index);

  protected:
    RingBuffer *getRingBuf(void) { return ringBuffer; }

    int AutoSelectTrack(uint type) override; // DecoderBase

    void ScanATSCCaptionStreams(int av_stream_index);
    void UpdateATSCCaptionTracks(void);
    void UpdateCaptionTracksFromStreams(bool check_608, bool check_708);
    void ScanTeletextCaptions(int av_stream_index);
    void ScanRawTextCaptions(int av_stream_index);
    void ScanDSMCCStreams(void);
    int  AutoSelectAudioTrack(void);
    int  filter_max_ch(const AVFormatContext *ic,
                       const sinfo_vec_t     &tracks,
                       const vector<int>     &fs,
                       enum AVCodecID         codecId = AV_CODEC_ID_NONE,
                       int                    profile = -1);

    friend int get_avf_buffer(struct AVCodecContext *c, AVFrame *pic,
                              int flags);
    friend int get_avf_buffer_vaapi2(struct AVCodecContext *c, AVFrame *pic,
                              int flags);
    friend int get_avf_buffer_nvdec(struct AVCodecContext *c, AVFrame *pic,
                              int flags);
    friend void release_avf_buffer(void *opaque, uint8_t *data);

    friend int open_avf(URLContext *h, const char *filename, int flags);
    friend int read_avf(URLContext *h, uint8_t *buf, int buf_size);
    friend int write_avf(URLContext *h, uint8_t *buf, int buf_size);
    friend int64_t seek_avf(URLContext *h, int64_t offset, int whence);
    friend int close_avf(URLContext *h);

    void DecodeDTVCC(const uint8_t *buf, uint buf_size, bool scte);
    void DecodeCCx08(const uint8_t *buf, uint buf_size, bool scte);
    void InitByteContext(bool forceseek = false);
    void InitVideoCodec(AVStream *stream, AVCodecContext *enc,
                        bool selectedStream = false);

    /// Preprocess a packet, setting the video parms if necessary.
    void MpegPreProcessPkt(AVStream *stream, AVPacket *pkt);
    int  H264PreProcessPkt(AVStream *stream, AVPacket *pkt);
    bool PreProcessVideoPacket(AVStream *stream, AVPacket *pkt);
    virtual bool ProcessVideoPacket(AVStream *stream, AVPacket *pkt);
    virtual bool ProcessVideoFrame(AVStream *stream, AVFrame *mpa_pic);
    bool ProcessAudioPacket(AVStream *stream, AVPacket *pkt,
                            DecodeType decodetype);
    bool ProcessSubtitlePacket(AVStream *stream, AVPacket *pkt);
    bool ProcessRawTextPacket(AVPacket *pkt);
    virtual bool ProcessDataPacket(AVStream *curstream, AVPacket *pkt,
                                   DecodeType decodetype);

    void ProcessVBIDataPacket(const AVStream *stream, const AVPacket *pkt);
    void ProcessDVBDataPacket(const AVStream *stream, const AVPacket *pkt);
    void ProcessDSMCCPacket(const AVStream *stream, const AVPacket *pkt);

    float GetMpegAspect(AVCodecContext *context, int aspect_ratio_info,
                        int width, int height);

    void SeekReset(long long, uint skipFrames, bool doFlush, bool discardFrames) override; // DecoderBase

    inline bool DecoderWillDownmix(const AVCodecContext *ctx);
    bool DoPassThrough(const AVCodecParameters *ctx, bool withProfile=true);
    bool SetupAudioStream(void);
    void SetupAudioStreamSubIndexes(int streamIndex);
    void RemoveAudioStreams();

    /// Update our position map, keyframe distance, and the like.
    /// Called for key frame packets.
    void HandleGopStart(AVPacket *pkt, bool can_reliably_parse_keyframes);

    bool GenerateDummyVideoFrames(void);
    bool HasVideo(const AVFormatContext *ic);
    float normalized_fps(AVStream *stream, AVCodecContext *enc);
    void av_update_stream_timings_video(AVFormatContext *ic);
    bool OpenAVCodec(AVCodecContext *avctx, const AVCodec *codec);

    void UpdateFramesPlayed(void) override; // DecoderBase
    bool DoRewindSeek(long long desiredFrame) override; // DecoderBase
    void DoFastForwardSeek(long long desiredFrame, bool &needflush) override; // DecoderBase
    virtual void StreamChangeCheck(void);
    virtual void PostProcessTracks(void) { }
    virtual bool IsValidStream(int /*streamid*/) {return true;}

    int DecodeAudio(AVCodecContext *ctx, uint8_t *buffer, int &data_size,
                    AVPacket *pkt);

    virtual int ReadPacket(AVFormatContext *ctx, AVPacket *pkt, bool &storePacket);

    PrivateDecoder *private_dec;

    bool is_db_ignored;

    H264Parser *m_h264_parser;

    AVFormatContext *ic;
    // AVFormatParameters params;

    URLContext readcontext;

    int frame_decoded;
    VideoFrame *decoded_video_frame;
    AVFRingBuffer *avfRingBuffer;

    struct SwsContext *sws_ctx;
    bool directrendering;

    bool no_dts_hack;
    bool dorewind;

    bool gopset;
    /// A flag to indicate that we've seen a GOP frame.  Used in junction with seq_count.
    bool seen_gop;
    int seq_count; ///< A counter used to determine if we need to force a call to HandleGopStart

    QList<AVPacket*> storedPackets;

    int prevgoppos;

    // GetFrame
    bool gotVideoFrame;
    bool hasVideo;
    bool needDummyVideoFrames;
    bool skipaudio;
    bool allowedquit;

    uint32_t  start_code_state;

    long long lastvpts;
    long long lastapts;
    long long lastccptsu;
    long long firstvpts;
    bool      firstvptsinuse;

    int64_t faulty_pts;
    int64_t faulty_dts;
    int64_t last_pts_for_fault_detection;
    int64_t last_dts_for_fault_detection;
    bool pts_detected;
    bool reordered_pts_detected;
    bool pts_selected;
    // set use_frame_timing true to utilize the pts values in returned
    // frames. Set fale to use deprecated method.
    bool use_frame_timing;

    bool force_dts_timestamps;

    PlayerFlags playerFlags;
    MythCodecID video_codec_id;

    int maxkeyframedist;
    int averror_count;

    // Caption/Subtitle/Teletext decoders
    uint             ignore_scte;
    uint             invert_scte_field;
    uint             last_scte_field;
    CC608Decoder     *ccd608;
    CC708Decoder     *ccd708;
    TeletextDecoder  *ttd;
    int               cc608_parity_table[256];
    /// Lookup table for whether a stream was seen in the PMT
    /// entries 0-3 correspond to CEA-608 CC1 through CC4, while
    /// entries 4-67 corresport to CEA-708 streams 0 through 64
    bool              ccX08_in_pmt[64+4];
    /// Lookup table for whether a stream is represented in the UI
    /// entries 0-3 correspond to CEA-608 CC1 through CC4, while
    /// entries 4-67 corresport to CEA-708 streams 0 through 64
    bool              ccX08_in_tracks[64+4];
    /// StreamInfo for 608 and 708 Captions seen in the PMT descriptor
    QList<StreamInfo> pmt_tracks;
    /// TrackType (608 or 708) for Captions seen in the PMT descriptor
    QList<TrackType>  pmt_track_types;
    /// StreamInfo for 608 and 708 Captions seen in the caption stream itself
    /// but not seen in the PMT
    QList<StreamInfo> stream_tracks;
    /// TrackType (608 or 708) for Captions seen in the caption stream itself
    /// but not seen in the PMT
    QList<TrackType>  stream_track_types;

    // MHEG
    InteractiveTV    *itv;                ///< MHEG/MHP decoder

    // Audio
    uint8_t          *audioSamples;
    bool              disable_passthru;

    AudioInfo         audioIn;
    AudioInfo         audioOut;

    float m_fps;
    bool  codec_is_mpeg;
    bool  m_processFrames;
    bool  m_streams_changed;
    // Value in milliseconds, from setting AudioReadAhead
    int m_audioReadAhead;
};

#endif

/* vim: set expandtab tabstop=4 shiftwidth=4: */
