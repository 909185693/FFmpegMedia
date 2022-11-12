// Fill out your copyright notice in the Description page of Project Settings.


#include "Player/FFmpegMediaTracks.h"
#include "FFmpegMedia.h"
#include "Internationalization/Internationalization.h"
#include "MediaHelpers.h"
#include "IMediaEventSink.h"
#include "FFmpegMediaBinarySample.h"
#include "FFmpegMediaOverlaySample.h"
#include "FFmpegMediaAudioSample.h"
#include "FFmpegMediaTextureSample.h"


 /* Minimum SDL audio buffer size, in samples. */
#define AUDIO_MIN_BUFFER_SIZE 512
/* Calculate actual buffer size keeping in mind not cause too frequent audio callbacks */
#define AUDIO_MAX_CALLBACKS_PER_SEC 30
 /* we use about AUDIO_DIFF_AVG_NB A-V differences to make the average */
#define AUDIO_DIFF_AVG_NB   20
 /* no AV correction is done if too big error */
#define AV_NOSYNC_THRESHOLD 9.0
#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
#define MIN_FRAMES 25
#define EXTERNAL_CLOCK_MIN_FRAMES 2
#define EXTERNAL_CLOCK_MAX_FRAMES 10
 /* polls for possible required screen refresh at least this often, should be less than 1/fps */
#define REFRESH_RATE 0.01
 /* external clock speed adjustment constants for realtime sources based on buffer fullness */
#define EXTERNAL_CLOCK_SPEED_MIN  0.900
#define EXTERNAL_CLOCK_SPEED_MAX  1.010
#define EXTERNAL_CLOCK_SPEED_STEP 0.001
 /* no AV sync correction is done if below the minimum AV sync threshold */
#define AV_SYNC_THRESHOLD_MIN 0.04
/* AV sync correction is done if above the maximum AV sync threshold */
#define AV_SYNC_THRESHOLD_MAX 0.1

 /* If a frame duration is longer than this, it will not be duplicated to compensate AV sync */
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.1
 /* maximum audio speed change to get correct sync */
#define SAMPLE_CORRECTION_PERCENT_MAX 10


#define LOCTEXT_NAMESPACE "FFmpegMediaTracks"

/**
 * 注意同一个Player实例只会初始化一次
 * 所以在构造器中初始化完成之后
 * Shutdown中必须完成部分数据初始化以及部分数据的重置，保证下次播放时为初始状态
 */
FFFmpegMediaTracks::FFFmpegMediaTracks()
{
    /************************* Player相关变量初始化 *********************************/
    this->CurrentState = EMediaState::Closed; //初始状态为Closed
    this->MediaSourceChanged = false;
    this->SelectionChanged = false;

    this->SelectedAudioTrack = INDEX_NONE; 
    this->SelectedCaptionTrack = INDEX_NONE;
    this->SelectedVideoTrack = INDEX_NONE;
   
    this->CurrentTime = FTimespan::Zero();// 当前播放时间
    this->CurrentRate = 0.0f; //当前播放速率

    this->ShouldLoop = false; //循环播放(注意该变量不需要重置)

    this->displayRunning = false;
    this->displayThread = nullptr;
    this->audioRunning = false;
    this->audioRenderThread = nullptr;

    this->AudioSamplePool = new FFFmpegMediaAudioSamplePool();
    this->VideoSamplePool = new FFFmpegMediaTextureSamplePool();

    this->currentOpenStreamNumber = 0;
    this->streamTotalNumber = 0;
    /***********************************************************************************/

    /************************* ffmpeg相关变量初始化 *********************************/
    //Initialize中有部分变量会初始化，所以构造函数和Shutdown中就不需要初始化
     auddec = MakeShareable(new FFmpegDecoder()); //音频解码器
     viddec = MakeShareable(new FFmpegDecoder()); //视频解码器
     subdec = MakeShareable(new FFmpegDecoder()); //字幕解码器


     //读取线程相关参数
     this->start_time = AV_NOPTS_VALUE; //当前不值seek, 该值固定
     this->abort_request = 0; //未中断
     this->paused = 0;// 停止
     this->last_paused = 0; //最后停止状态
     this->continue_read_thread = new FFmpegCond();
     this->seek_req = 0; //是否为seek请求
     this->seek_pos = 0; //seek位置
     this->seek_rel = 0; 
     this->seek_flags = 0;
     this->queue_attachments_req = 0;
     this->eof = 0;
     this->read_pause_return = 0;
   
     //stream open相关参数
     this->audio_st = nullptr;
     this->video_st = nullptr;
     this->subtitle_st = nullptr;


     //音频解码相关参数
     this->audio_buf = NULL;
     this->audio_buf1 = NULL;
     this->audio_buf1_size = 0;
     this->audio_buf_size = 0;
     this->audio_hw_buf_size = 0;
     this->audio_diff_cum = 0;


     //视频解码相关参数
     this->rdft = NULL; //无用
     this->rdft_bits = 0; //无用
     this->rdft_data = NULL; //无用
     this->swr_ctx = NULL;
     this->img_convert_ctx = NULL;
    
     //音频渲染
     this->audio_callback_time = 0;

     //视频渲染
     this->frame_timer = 0; //当前已经播放的帧的开始显示时间
     this->force_refresh = 0; //画面强制刷新
     this->frame_drops_late = 0; //统计视频播放时丢弃的帧数量
     this->step = 0;//逐帧播放  默认为0，没有实现 废弃
     this->frame_drops_early = 0;

     this->muted  = 0;
     this->frame_last_filter_delay = 0;
     this->LastFetchVideoTime = 0;
}

FFFmpegMediaTracks::~FFFmpegMediaTracks()
{
}

void FFFmpegMediaTracks::Initialize(AVFormatContext* ic_, const FString& Url, const FMediaPlayerOptions* PlayerOptions)
{
    UE_LOG(LogFFmpegMedia, Verbose, TEXT("Tracks: %p: Initializing ..."), this);
    
    if (PlayerOptions != nullptr)
    {
        this->MediaTrackOptions = PlayerOptions->Tracks;
    }

    FScopeLock Lock(&CriticalSection); //注意加锁
 
    //初始化设置媒体是否改变和轨道是否改变为true
    this->MediaSourceChanged = true;
    this->SelectionChanged = true;

    if (!ic_)
    {
        UE_LOG(LogFFmpegMedia, Error, TEXT("Tracks: %p: Initialize fail, Because FormatContext null"), this);
        //设置当前状态为错误
        CurrentState = EMediaState::Error;
        return;
    }
    this->ic = ic_;

    //参考stream_open，先初始化
    this->last_video_stream = this->video_stream = -1;
    this->last_audio_stream = this->audio_stream = -1;
    this->last_subtitle_stream = this->subtitle_stream = -1;

    /* start video display */
    if (this->pictq.Init(&this->videoq, VIDEO_PICTURE_QUEUE_SIZE, 1) < 0) {  //初始化图片解码帧队列
        UE_LOG(LogFFmpegMedia, Error, TEXT("Tracks: %p: Initialize fail, Because pictq init fail"), this);
        goto fail;
    }
    UE_LOG(LogFFmpegMedia, Verbose, TEXT("Tracks: %p: Initializing pictq frame queue success"), this);

    if (this->subpq.Init(&this->subtitleq, SUBPICTURE_QUEUE_SIZE, 0) < 0) { // //初始化字幕解码帧队列
        UE_LOG(LogFFmpegMedia, Error, TEXT("Tracks: %p: Initialize fail, Because subpq init fail"), this);
        goto fail;
    }
    UE_LOG(LogFFmpegMedia, Verbose, TEXT("Tracks: %p: Initializing subpq frame queue success"), this);

    if (this->sampq.Init(&this->audioq, SAMPLE_QUEUE_SIZE, 1) < 0) {   //初始化音频解码帧队列
        UE_LOG(LogFFmpegMedia, Error, TEXT("Tracks: %p: Initialize fail, Because sampq init fail"), this);
        goto fail;
    }
    UE_LOG(LogFFmpegMedia, Verbose, TEXT("Tracks: %p: Initializing audioq frame queue success"), this);

    if (this->videoq.Init() < 0 ||
    this->audioq.Init() < 0 ||
    this->subtitleq.Init() < 0)
        goto fail;

    UE_LOG(LogFFmpegMedia, Verbose, TEXT("Tracks: %p: Initializing videoq audioq subtitleq packet queue success"), this);
   
    this->continue_read_thread = new FFmpegCond();
    if (!this->continue_read_thread) {
        UE_LOG(LogFFmpegMedia, Error, TEXT("Tracks: %p:  create continue_read_thread fail"), this);
        goto fail;
    }
     
    this->vidclk.Init(&this->videoq);
    this->audclk.Init(&this->audioq);
    this->extclk.Init(&this->extclk);
    UE_LOG(LogFFmpegMedia, Verbose, TEXT("Tracks: %p: Initializing vidclk audclk extclk success"), this);

    //读取流之前，将状态设置为准备状态
    CurrentState = EMediaState::Preparing;

    this->audio_clock_serial = -1;
    int startup_volume = 100; //声音范围 set startup volume 0=min 100=max
    if (startup_volume < 0)
        UE_LOG(LogFFmpegMedia, Verbose, TEXT("Tracks: %p:  -volume=%d < 0, setting to 0"), this, startup_volume);
    if (startup_volume > 100)
        UE_LOG(LogFFmpegMedia, Verbose, TEXT("Tracks: %p:  -volume=%d > 100, setting to 100"), this, startup_volume);
    startup_volume = av_clip(startup_volume, 0, 100);
    this->audio_volume = startup_volume;
    this->muted = 0;
    this->av_sync_type = AV_SYNC_AUDIO_MASTER; //默认音频(此时同步只会使用音频时钟和外部时钟)

    unsigned  i;
    for (i = 0; i < ic_->nb_streams; i++) {
        AVStream* st = ic->streams[i];
        enum AVMediaType type = st->codecpar->codec_type;
        st->discard = AVDISCARD_ALL;
        //将ffmpeg中的流转化成轨道信息
        bool streamAdded = this->AddStreamToTracks(i, this->MediaTrackOptions, this->MediaInfo);
        if (streamAdded) {
            //统计流总数量
            this->streamTotalNumber++;
            UE_LOG(LogFFmpegMedia, Verbose, TEXT("Tracks: %p: add streacm %d success"), this, i);
        }
    }

    this->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;  ////设置帧最大时长

    //设置总时长
    int64_t duration = ic->duration; //微秒us
    this->Duration = duration *10; //转化必须乘以10，否则时间不对
    this->realtime = is_realtime(ic);

    DeferredEvents.Enqueue(EMediaEvent::MediaOpened); //发送事件，会触发SetRate(1.0f);
    //启动ReadThread
    this->read_tid = LambdaFunctionRunnable::RunThreaded(TEXT("ReadThread"), [this] {
        read_thread();
    });
    if (!this->read_tid) {
        UE_LOG(LogFFmpegMedia, Error, TEXT("Tracks: %p:  start read thread fail"), this);
        goto fail;
    }
    UE_LOG(LogFFmpegMedia, Verbose, TEXT("Tracks: %p: start read thread[%d] success"), this, read_tid->GetThreadID());
    UE_LOG(LogFFmpegMedia, Log, TEXT("Tracks: %p: Initializing success"), this);
    return;
fail:
    CurrentState = EMediaState::Error;
    UE_LOG(LogFFmpegMedia, Verbose, TEXT("Tracks: %p: Initializing fail"), this);
    return;
}

/** 
* 注意: UE4中ShutDown与ffplay中不一样，UE4中相当于重置到停止状态，需要复用的 
* 按照先打开后关闭的原则进行资源的回收或者重置
* 为了保证回到初始化状态，除个别变量，其他所有变量都要
*/
void FFFmpegMediaTracks::Shutdown()
{
    UE_LOG(LogFFmpegMedia, Verbose, TEXT("Tracks: %p: Shutting down ..."), this);

    /** 首选中断读取线程 */
    this->abort_request = 1;
    //中断displayThread线程
    this->displayRunning = false;
    //中断audioThread线程
    this->audioRunning = false;
    //关闭读取线程
    if (this->read_tid != nullptr) {
        this->read_tid->WaitForCompletion();
    }

    //关闭各个流
    /* close each stream */
    if (this->audio_stream >= 0)
        this->stream_component_close(this->audio_stream);
    if (this->video_stream >= 0)
        this->stream_component_close(this->video_stream);
    if (this->subtitle_stream >= 0)
        this->stream_component_close(this->subtitle_stream);

    //avformat_close_input(&this->ic); //销毁上下文
    if (this->ic) {
        avformat_close_input(&ic);
        this->ic = nullptr;
    }

    //销毁包队列
    this->videoq.Destroy();
    this->audioq.Destroy();
    this->subtitleq.Destroy();

    //销毁帧队列
    /* free all pictures */
    this->pictq.Destory();
    this->sampq.Destory();
    this->subpq.Destory();
    if (img_convert_ctx != nullptr) {
        sws_freeContext(this->img_convert_ctx);
        this->img_convert_ctx = NULL;
    }
    //sws_freeContext(this->sub_convert_ctx);

    //销毁线程
    //关闭display线程
    if (displayThread != nullptr) {
        displayThread->WaitForCompletion();
        displayThread = nullptr;
    }
    if (audioRenderThread != nullptr) {
        audioRenderThread->WaitForCompletion();
        audioRenderThread = nullptr;
    }

    FScopeLock Lock(&CriticalSection);
    /************************* Player相关变量初始化 *********************************/
    this->CurrentState = EMediaState::Closed; //初始状态为Closed
    this->MediaSourceChanged = false;
    this->SelectionChanged = false;

    this->SelectedAudioTrack = INDEX_NONE;
    this->SelectedCaptionTrack = INDEX_NONE;
    this->SelectedVideoTrack = INDEX_NONE;

    this->CurrentTime = FTimespan::Zero();// 当前播放时间
    this->CurrentRate = 0.0f; //当前播放速率

    this->AudioSamplePool->Reset();
    this->VideoSamplePool->Reset();
    this->AudioTracks.Empty();
    this->CaptionTracks.Empty();
    this->VideoTracks.Empty();
    MediaInfo.Empty();
    ImgaeCopyDataBuffer.Reset();
    this->currentOpenStreamNumber = 0;
    this->streamTotalNumber = 0;
    /***********************************************************************************/

    /************************* ffmpeg相关变量初始化 *********************************/
    //Initialize中有部分变量会初始化，所以构造函数和Shutdown中就不需要初始化
    //读取线程相关参数
    this->start_time = AV_NOPTS_VALUE; //当前不值seek, 该值固定
    this->paused = 0;// 停止
    this->last_paused = 0; //最后停止状态
    this->seek_req = 0; //是否为seek请求
    this->seek_pos = 0; //seek位置
    this->seek_rel = 0;
    this->seek_flags = 0;
    this->queue_attachments_req = 0;
    this->eof = 0;
    this->read_pause_return = 0;

    //音频渲染
    this->audio_callback_time = 0;

    //视频渲染
    this->frame_timer = 0; //当前已经播放的帧的开始显示时间
    this->force_refresh = 0; //画面强制刷新
    this->frame_drops_late = 0; //统计视频播放时丢弃的帧数量
    this->step = 0;//逐帧播放  默认为0，没有实现 废弃
    this->frame_drops_early = 0;

    this->muted = 0;
    this->frame_last_filter_delay = 0;

    //重要设置,清理之后，将中断操作重置为0
    this->abort_request = 0;
    this->LastFetchVideoTime = 0;
}


/**
* 参考ffplay的read_thread方法实现
* 与ffplay中不同的是
* 该方法不会调用stream_component_open方法，stream_component_open会在SelectTrack中调用
* 会删除一切wanted_stream_spec相关代码，因为ue中，会根据选择的轨道选择读取指定的流，而非想ffplay中指定参数
*/
int FFFmpegMediaTracks::read_thread()
{
    int seek_by_bytes = -1; //seek by bytes 0=off 1=on -1=auto 默认为-1, UE中使用time_seek, 不会使用字节seek
    AVPacket* pkt = NULL;
    FCriticalSection* wait_mutex = new FCriticalSection();
    int ret;
    int64_t stream_start_time;
    int64_t pkt_ts;
    int pkt_in_play_range = 0;

    pkt = av_packet_alloc();
    if (!pkt) {
        UE_LOG(LogFFmpegMedia, Error, TEXT("Tracks %p: Could not allocate packet"), this);
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    if (seek_by_bytes < 0)
        seek_by_bytes = !(ic->iformat->flags & AVFMT_NO_BYTE_SEEK) &
        !!(ic->iformat->flags & AVFMT_TS_DISCONT) &
        strcmp("ogg", ic->iformat->name);
    UE_LOG(LogFFmpegMedia, Error, TEXT("Tracks %p: Could not allocate packet %d"), this, seek_by_bytes);
    int64_t duration = ic->duration; //时长

    //如果是点击跳转播放
    /* if seeking requested, we execute it */
    if (start_time != AV_NOPTS_VALUE) {
        int64_t timestamp;

        timestamp = start_time;
        /* add the stream start time */
        if (ic->start_time != AV_NOPTS_VALUE)
            timestamp += ic->start_time;
        //跳转到点击播放位置
        ret = avformat_seek_file(ic, -1, INT64_MIN, timestamp, INT64_MAX, 0);
        if (ret < 0) {
            UE_LOG(LogFFmpegMedia, Warning, TEXT("Tracks: %p: could not seek to position %0.3f"), this, (double)timestamp / AV_TIME_BASE);
        }
    }

    int infinite_buffer = -1; //不限制缓存 don't limit the input buffer size (useful with realtime streams)
    if (infinite_buffer < 0 && this->realtime)
        infinite_buffer = 1; //实时流时不限制

    //循环读取数据包，并放入队列中去
    for (;;) {
        if (this->abort_request)
            break;
        //保证流打开，则锁定等待10ms，有bug, 只需要保证所需流打开
        if (this->currentOpenStreamNumber < this->streamTotalNumber) {
            wait_mutex->Lock();
            continue_read_thread->waitTimeout(*wait_mutex, 10);
            wait_mutex->Unlock();
            continue;
        }
        //此处同步UE状态和ffmpeg状态，当播放器处于暂停状态和停止状态时，都将ffpemg停止状态设置成true
        this->paused = this->CurrentState == EMediaState::Paused || this->CurrentState == EMediaState::Stopped;
        if (this->paused != this->last_paused) {
            this->last_paused = this->paused;
            if (this->paused)
                this->read_pause_return = av_read_pause(ic);
            else
                av_read_play(ic);
        }
        if (this->seek_req) {
            int64_t seek_target = this->seek_pos; //seek目标位置
            int64_t seek_min = this->seek_rel > 0 ? seek_target - this->seek_rel + 2 : INT64_MIN;
            int64_t seek_max = this->seek_rel < 0 ? seek_target - this->seek_rel - 2 : INT64_MAX;
            // FIXME the +-2 is due to rounding being not done in the correct direction in generation
            //      of the seek_pos/seek_rel variables

            ret = avformat_seek_file(this->ic, -1, seek_min, seek_target, seek_max, this->seek_flags);
            if (ret < 0) {
                UE_LOG(LogFFmpegMedia, Error, TEXT("Tracks: %p:  %s: error while seeking"), this, this->ic->url);
            }
            else {
                if (this->audio_stream >= 0)
                    this->audioq.Flush();
                if (this->subtitle_stream >= 0)
                    this->subtitleq.Flush();
                if (this->video_stream >= 0)
                    this->videoq.Flush();
                if (this->seek_flags & AVSEEK_FLAG_BYTE) {
                    this->extclk.Set(NAN, 0);
                }
                else {
                    this->extclk.Set(seek_target / (double)AV_TIME_BASE, 0);
                }
                FlushSamples();
                DeferredEvents.Enqueue(EMediaEvent::SeekCompleted); //会触发FlushSamples()调用;
            }
            this->seek_req = 0;
            this->queue_attachments_req = 1;
            this->eof = 0;
            SetRate(1.0f);
            if (this->paused) {
                this->step_to_next_frame();
            }
        }
        if (this->queue_attachments_req) {
            if (this->video_st && this->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
                ret = av_packet_ref(pkt, &this->video_st->attached_pic);
                if (ret < 0) {
                    UE_LOG(LogFFmpegMedia, Error, TEXT("Tracks: %p:  read_thread av_packet_ref fail"), this);
                    continue;
                }
                this->videoq.Put(pkt);
                this->videoq.PutNullpacket(pkt, this->video_stream);
            }
            this->queue_attachments_req = 0;
        }
        /* if the queue are full, no need to read more */
        //如果非实时流，则限制3个队列大小为15M或者3个队列同时满了，等待10毫秒(如果只消费其中一个队列，则肯定会超过15M, 调试时注意)
        if (infinite_buffer < 1 &&
            (this->audioq.size + this->videoq.size + this->subtitleq.size > MAX_QUEUE_SIZE
                || (stream_has_enough_packets(this->audio_st, this->audio_stream, &this->audioq) &&
                    stream_has_enough_packets(this->video_st, this->video_stream, &this->videoq) &&
                    stream_has_enough_packets(this->subtitle_st, this->subtitle_stream, &this->subtitleq)))) {
            /* wait 10 ms */
            wait_mutex->Lock();
            continue_read_thread->waitTimeout(*wait_mutex, 10);
            wait_mutex->Unlock();
            continue;
        }
        //结束了，从头播放
        if (!this->paused &&
            (!this->audio_st || (this->auddec->GetFinished() == this->audioq.serial && this->sampq.NbRemaining() == 0)) &&
            (!this->video_st || (this->viddec->GetFinished() == this->videoq.serial && this->pictq.NbRemaining() == 0 ))) {
            
            if (this->get_master_sync_type() == AV_SYNC_AUDIO_MASTER) { //音频等待读取完，音频速度快，没播放完样本数一定大于0
                if (this->AudioSampleQueue.Num() != 0) {
                    continue;
                }
            }
            else {
                if (LastFetchVideoTime < Duration.GetTotalSeconds()) { //视频等待读取完，视频速度慢，判断最后一个样本时间是否小于时长
                    continue;
                }
            }
       
            if (this->ShouldLoop) { //只要设置循环，就重复播放，不是设置循环次数
                DeferredEvents.Enqueue(EMediaEvent::PlaybackEndReached);
                this->stream_seek(start_time != AV_NOPTS_VALUE ? start_time : 0, 0, 0);
            }
            else {
                if (this->eof) {
                    //结束
                    CurrentState = EMediaState::Stopped;
                    DeferredEvents.Enqueue(EMediaEvent::PlaybackEndReached);
                    DeferredEvents.Enqueue(EMediaEvent::PlaybackSuspended);
                }
            }
        }
        ret = av_read_frame(ic, pkt);
        if (ret < 0) {
            if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !this->eof) {
                if (this->video_stream >= 0)
                    this->videoq.PutNullpacket(pkt, this->video_stream);
                if (this->audio_stream >= 0)
                    this->audioq.PutNullpacket(pkt, this->audio_stream);
                if (this->subtitle_stream >= 0)
                    this->subtitleq.PutNullpacket(pkt, this->subtitle_stream);
                this->eof = 1;
            }
            if (ic->pb && ic->pb->error) {
                 UE_LOG(LogFFmpegMedia, Error, TEXT("Tracks: %p:  read_thread av_read_frame fail"), this);
                 continue;
            }
            wait_mutex->Lock();
            continue_read_thread->waitTimeout(*wait_mutex, 10);
            wait_mutex->Unlock();
            continue;
        }
        else {
            this->eof = 0;
        }
        /* check if packet is in play range specified by user, then queue, otherwise discard */
        stream_start_time = ic->streams[pkt->stream_index]->start_time;
        pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
        pkt_in_play_range = duration == AV_NOPTS_VALUE ||
            (pkt_ts - (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
            av_q2d(ic->streams[pkt->stream_index]->time_base) -
            (double)(start_time != AV_NOPTS_VALUE ? start_time : 0) / 1000000
            <= ((double)duration / 1000000);
        if (pkt->stream_index == this->audio_stream && pkt_in_play_range) {
            this->audioq.Put(pkt);
        }
        else if (pkt->stream_index == this->video_stream && pkt_in_play_range
            && !(this->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            this->videoq.Put(pkt);
        }
        else if (pkt->stream_index == this->subtitle_stream && pkt_in_play_range) {
            this->subtitleq.Put(pkt);
        }
        else {
            av_packet_unref(pkt);
        }
    }

    ret = 0;
fail:
    av_packet_free(&pkt);
    if (ret != 0) {
        CurrentState = EMediaState::Error;
        UE_LOG(LogFFmpegMedia, Error, TEXT("Tracks: %p:  read_thread run fail"), this);
    /*    char errbuf[1024] = {};
        av_strerror(ret, errbuf, 1024);
        UE_LOG(LogFFmpegMedia, Error, TEXT("Player %p: Couldn't Open File %d(%s)"), this, ret, errbuf);*/
    }
    else {
        UE_LOG(LogFFmpegMedia, Log, TEXT("Tracks: %p:  read_thread exit"), this);
    }
    return 0;
}

bool FFFmpegMediaTracks::AddStreamToTracks(uint32 StreamIndex, const FMediaPlayerTrackOptions& TrackOptions, FString& OutInfo)
{
    OutInfo += FString::Printf(TEXT("Stream %i\n"), StreamIndex);
    AVStream* StreamDescriptor = ic->streams[StreamIndex]; //获取ffmpeg上下文的流对象
    AVCodecParameters* CodecParams = StreamDescriptor->codecpar;
    AVMediaType MediaType = CodecParams->codec_type;

    //判断流是否为视频流、音频流和字幕流
    if (MediaType != AVMEDIA_TYPE_VIDEO
        && MediaType != AVMEDIA_TYPE_AUDIO
        && MediaType != AVMEDIA_TYPE_SUBTITLE) {
        UE_LOG(LogFFmpegMedia, Verbose, TEXT("Tracks %p: Unsupported major type %s of stream %i"), this, av_get_media_type_string(MediaType), StreamIndex);
        OutInfo += TEXT("\tUnsupported stream type\n");
        return false;
    }


    //创建和添加轨道
    FTrack* Track = nullptr;
    int32 TrackIndex = INDEX_NONE;
    int32* SelectedTrack = nullptr;

    //如果是音频类型
    if (MediaType == AVMEDIA_TYPE_AUDIO)
    {
        SelectedTrack = &SelectedAudioTrack; //设置当前轨道为选择的音频轨道
        TrackIndex = AudioTracks.AddDefaulted();//音频轨道添加一个默认轨道
        Track = &AudioTracks[TrackIndex]; //取出添加的默认轨道
        //SelectedAudioTrack = TrackIndex;
    }
    //如果是字幕类型
    else if (MediaType == AVMEDIA_TYPE_SUBTITLE)
    {
        SelectedTrack = &SelectedCaptionTrack;
        TrackIndex = CaptionTracks.AddDefaulted();
        Track = &CaptionTracks[TrackIndex];
    }
    //如果是视频类型
    else if (MediaType == AVMEDIA_TYPE_VIDEO)
    {
        SelectedTrack = &SelectedVideoTrack;
        TrackIndex = VideoTracks.AddDefaulted();
        Track = &VideoTracks[TrackIndex];
        //SelectedVideoTrack = TrackIndex;
    }

    //检查添加的轨道是否为空
    check(Track != nullptr);
    //检查添加轨道的索引是否为空
    check(TrackIndex != INDEX_NONE);
    //检查当前选择的轨道是否空
    check(SelectedTrack != nullptr);

    const FString TypeName = avcodec_get_name(CodecParams->codec_id);
    OutInfo += FString::Printf(TEXT("\t\tCodec: %s\n"), *TypeName);
    //设置轨道语言
    if (this->ic->metadata) {
        AVDictionaryEntry* t = av_dict_get(this->ic->metadata, "language", NULL, 0);
        if (t) {
            Track->Language = t->value;
        }
    }

    if (MediaType == AVMEDIA_TYPE_AUDIO)
    {
        int samples = FFMAX(AUDIO_MIN_BUFFER_SIZE, 2 << av_log2(CodecParams->sample_rate / AUDIO_MAX_CALLBACKS_PER_SEC));
        
        Track->Format = {
            MediaType,
            CodecParams->codec_id,
            TypeName,
            {
                (uint32)av_samples_get_buffer_size(NULL, CodecParams->channels, 1, AV_SAMPLE_FMT_S16, 1),
                (uint32)CodecParams->channels,
                (uint32)CodecParams->sample_rate,
                //(AVChannelLayout)CodecParams->channel_layout,
                CodecParams->ch_layout,
                AV_SAMPLE_FMT_S16,
                (uint32)av_samples_get_buffer_size(NULL, CodecParams->channels, CodecParams->sample_rate,  AV_SAMPLE_FMT_S16, 1),
                (uint32)av_samples_get_buffer_size(NULL, CodecParams->channels, samples, AV_SAMPLE_FMT_S16, 1)
            },
            {0}
        };

        OutInfo += FString::Printf(TEXT("\t\tChannels: %i\n"), Track->Format.Audio.NumChannels);
        OutInfo += FString::Printf(TEXT("\t\tSample Rate: %i Hz\n"), Track->Format.Audio.SampleRate);
        OutInfo += FString::Printf(TEXT("\t\tBits Per Sample: %i\n"), Track->Format.Audio.FrameSize * 8);
    }
    else if (MediaType == AVMEDIA_TYPE_VIDEO) {

        float fps = av_q2d(StreamDescriptor->r_frame_rate);
        if (fps < 0.000025) {
            fps = av_q2d(StreamDescriptor->avg_frame_rate);
        }

        OutInfo += FString::Printf(TEXT("\t\tFrame Rate: %g fps\n"), fps);

        int line_sizes[4] = { 0 };
        av_image_fill_linesizes(line_sizes, (AVPixelFormat)CodecParams->format, CodecParams->width);

        FIntPoint OutputDim = { CodecParams->width, CodecParams->height };

        OutInfo += FString::Printf(TEXT("\t\tDimensions: %i x %i\n"), OutputDim.X, OutputDim.Y);

        Track->Format = {
            MediaType,
            CodecParams->codec_id,
            TypeName,
            {
               0
            },
            {
                0
            }
        };

        Track->Format.Video.BitRate = CodecParams->bit_rate;
        Track->Format.Video.OutputDim = OutputDim;
        Track->Format.Video.FrameRate = fps;
        Track->Format.Video.LineSize[0] = line_sizes[0];
        Track->Format.Video.LineSize[1] = line_sizes[1];
        Track->Format.Video.LineSize[2] = line_sizes[2];
        Track->Format.Video.LineSize[3] = line_sizes[3];

    }
    else {
        Track->Format = {
            MediaType,
            CodecParams->codec_id,
            TypeName,
            {
               0
            },
            {
               0
            }
        };
    }

    //设置轨道名称
    Track->DisplayName = (Track->Name.IsEmpty())
        ? FText::Format(LOCTEXT("UnnamedStreamFormat", "Unnamed Track (Stream {0})"), FText::AsNumber((uint32)StreamIndex))
        : FText::FromString(Track->Name);

    Track->StreamIndex = StreamIndex; //设置轨道索引为流索引
    if (MediaType == AVMEDIA_TYPE_SUBTITLE && TrackOptions.Caption == INDEX_NONE) {
        return false;
    }
    return true;
}

void FFFmpegMediaTracks::ClearFlags()
{
    FScopeLock Lock(&CriticalSection);

    MediaSourceChanged = false; //媒体是否改变
    SelectionChanged = false; //轨道是否改变
}

/** 显示线程 相当于 refresh_loop_wait_event */
int FFFmpegMediaTracks::DisplayThread()
{
    double remaining_time = 0.0; //播放下一帧需要等待的时间，单位秒
    //判断显示运行状态
    while (displayRunning) {
        if (remaining_time > 0.0)
            av_usleep((int64_t)(remaining_time * 1000000.0)); //睡眠一段时间，防止无意义的频繁调用
        remaining_time = REFRESH_RATE; //默认屏幕刷新率控制，REFRESH_RATE = 10ms
        if (!this->paused || this->force_refresh)
            video_refresh(&remaining_time);
    }
    UE_LOG(LogFFmpegMedia, Log, TEXT("Tracks: %p:  DisplayThread exit"), this);
    return 0;
}

int FFFmpegMediaTracks::AudioRenderThread() {
    double remaining_time = 0.0;
    int64_t startTime = av_gettime_relative(); //初始化开始渲染时间
    while (audioRunning) {
        if (this->paused) { //添加是否停止判断
            continue; 
        }
        FTimespan duration = RenderAudio();
        int64_t endTime = av_gettime_relative(); //初始化渲染结束时间
        int64_t dif = endTime - startTime; //计算渲染时间
        int64_t range = 30000;
        if (dif < range) { //重要代码: 用于保证每隔一定时间, 将一个音频样本放入队列中去, 而不是一直放入队列中，减少内存的消耗，注意range的值不能过大
            av_usleep(range - dif);
        }
        startTime = endTime;
    }
    UE_LOG(LogFFmpegMedia, Log, TEXT("Tracks: %p:  AudioRenderThread exit"), this);
    return 0;
}

//视频刷新
void FFFmpegMediaTracks::video_refresh(double* remaining_time)
{
    double time;
    double rdftspeed = 0.02;
    FFmpegFrame* sp, * sp2;

    if (!this->paused && this->get_master_sync_type() == AV_SYNC_EXTERNAL_CLOCK && this->realtime)
        this->check_external_clock_speed(); //同步外部时钟

    //此处删除音频波形显示代码
    if (this->video_st) {
retry:
        if (this->pictq.NbRemaining() == 0) { //判断是否有剩余帧
            // nothing to do, no picture to display in the queue
        }
        else {
            double last_duration, duration, delay; //
            FFmpegFrame* vp, * lastvp;

            /* dequeue the picture */
            lastvp = this->pictq.PeekLast(); //上一帧(正在显示的)
            vp = this->pictq.Peek(); //当前帧(正要显示的)
            if (this->get_master_sync_type() == AV_SYNC_VIDEO_MASTER) {
                CurrentTime = FTimespan::FromSeconds(vp->GetPts());
            }
            else if (this->get_master_sync_type() == AV_SYNC_EXTERNAL_CLOCK) {
                CurrentTime = FTimespan::FromSeconds(extclk.Get());
            }

            if (vp->GetSerial() != this->videoq.serial) { //丢弃无效的Frame
                UE_LOG(LogFFmpegMedia, Error, TEXT("Player %p: drop a video frame"), this);
                this->pictq.Next();
                goto retry;
            }

            if (lastvp->GetSerial() != vp->GetSerial()) 
                this->frame_timer = av_gettime_relative() / 1000000.0; //设置当前帧显示的时间

            if (this->paused)//暂停状态
                goto display;

            /* compute nominal last_duration */
            last_duration = vp_duration(lastvp, vp); //获取上一帧需要显示的时长
            delay = compute_target_delay(last_duration); //计算上一帧还需要播放的时长

            time = av_gettime_relative() / 1000000.0;
            if (time < this->frame_timer + delay) { //如果当前时刻<当前画面显示完成的时间，表示画面还在显示中，计算剩余时间
                *remaining_time = FFMIN(this->frame_timer + delay - time, *remaining_time);
                goto display;
            }

            this->frame_timer += delay; //设置当前帧的显示时间(上一帧显示时间 + 上一帧时长)
            if (delay > 0 && time - this->frame_timer > AV_SYNC_THRESHOLD_MAX) //如果系统时间与当前帧的显示时长大于0.1，则重置frame_timer为系统时间 
                this->frame_timer = time;

            this->pictq.GetMutex()->Lock();
            if (!isnan(vp->pts)) {
                this->update_video_pts(vp->pts, vp->pos, vp->serial);
            }
            this->pictq.GetMutex()->Unlock();

            //丢弃帧的逻辑
            int framedrop = -1;//todo:
            if (this->pictq.NbRemaining() > 1) {
                FFmpegFrame* nextvp = this->pictq.PeekNext();
                duration = this->vp_duration(vp, nextvp);
                //重要判断time > this->frame_timer + duration，检查播放的帧是否已经过期
                if (!this->step && (framedrop > 0 || (framedrop && this->get_master_sync_type() != AV_SYNC_VIDEO_MASTER)) && time > this->frame_timer + duration) {
                    this->frame_drops_late++;
                    this->pictq.Next();
                    goto retry;
                }
            }
            //字幕处理
            if (this->subtitle_st) {
                while (this->subpq.NbRemaining() > 0) {
                    sp = this->subpq.Peek();

                    if (this->subpq.NbRemaining() > 1)
                        sp2 = this->subpq.PeekNext();
                    else
                        sp2 = NULL;

                    if (sp->GetSerial() != this->subtitleq.serial
                        || (this->vidclk.GetPts() > (sp->GetPts() + ((float)sp->GetSub().end_display_time / 1000)))
                        || (sp2 && this->vidclk.GetPts() > (sp2->GetPts() + ((float)sp2->GetSub().start_display_time / 1000))))
                    {
                       /* if (sp->GetUploaded()) {
                            int i;
                            for (i = 0; i < sp->GetSub().num_rects; i++) {
                                AVSubtitleRect* sub_rect = sp->GetSub().rects[i];
                                uint8_t* pixels;
                                int pitch, j;

                                if (!SDL_LockTexture(is->sub_texture, (SDL_Rect*)sub_rect, (void**)&pixels, &pitch)) {
                                    for (j = 0; j < sub_rect->h; j++, pixels += pitch)
                                        memset(pixels, 0, sub_rect->w << 2);
                                    SDL_UnlockTexture(is->sub_texture);
                                }
                            }
                        }*/
                        this->subpq.Next();
                    }
                    else {
                        break;
                    }
                }
            }

            this->pictq.Next();
            this->force_refresh = 1; //强制刷新
            if (this->step && !this->paused) //todo: 暂时没有逐帧播放功能
                this->stream_toggle_pause();
        }
 display:
        /* display picture 注意retry中force_refresh的控制，决定是否显示画面*/ 
        if (this->force_refresh && this->pictq.GetRindexShown())
            video_display();
    }
    this->force_refresh = 0; //重置强制刷新状态
}

void FFFmpegMediaTracks::check_external_clock_speed()
{
    if (this->video_stream >= 0 && this->videoq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES ||
        this->audio_stream >= 0 && this->audioq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES) {
        this->extclk.SetSpeed(FFMAX(EXTERNAL_CLOCK_SPEED_MIN, this->extclk.GetSpeed() - EXTERNAL_CLOCK_SPEED_STEP));
    }
    else if ((this->video_stream < 0 || this->videoq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES) &&
        (this->audio_stream < 0 || this->audioq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES)) {
        this->extclk.SetSpeed(FFMIN(EXTERNAL_CLOCK_SPEED_MAX, this->extclk.GetSpeed() + EXTERNAL_CLOCK_SPEED_STEP));
    }
    else {
        double speed = this->extclk.GetSpeed();
        if (speed != 1.0) {
            this->extclk.SetSpeed(speed + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed) / fabs(1.0 - speed));
        }
    }
}

void FFFmpegMediaTracks::GetEvents(TArray<EMediaEvent>& OutEvents)
{
    EMediaEvent Event;

    while (DeferredEvents.Dequeue(Event))
    {
        OutEvents.Add(Event);
    }
}

//删除一切与len参数相关的变量以及逻辑
//因为UE中没有对Len的要求
FTimespan FFFmpegMediaTracks::RenderAudio()
{
    int audio_size, len1;
    
    this->audio_callback_time = av_gettime_relative();
    FTimespan time = 0; //帧显示时间，解码之后获取
    FTimespan duration = 0; //帧时长，解码之后获取
    audio_size = this->audio_decode_frame(time, duration);
    if (audio_size < 0) {
        /* if error, just output silence */
        this->audio_buf = NULL;
        this->audio_buf_size = AUDIO_MIN_BUFFER_SIZE / this->audio_tgt.FrameSize * this->audio_tgt.FrameSize;
    }
    else {
        this->audio_buf_size = audio_size;
    }

    //this->audio_buf_index = 0;
    len1 = this->audio_buf_size;// -this->audio_buf_index;

    if (CurrentState == EMediaState::Paused || CurrentState == EMediaState::Stopped) {
        //Ignore the frame
        //audio_decode_frame 中
    }
    else {
        if (this->audio_buf != NULL) {
            FScopeLock Lock(&CriticalSection);
            const TSharedRef<FFFmpegMediaAudioSample, ESPMode::ThreadSafe> AudioSample = AudioSamplePool->AcquireShared();
            if (AudioSample->Initialize((uint8_t*)this->audio_buf, len1, audio_tgt.NumChannels, audio_tgt.SampleRate, time, duration))
            {
                AudioSampleQueue.Enqueue(AudioSample);
            }
        }
    }
    /* Let's assume the audio driver that is used by SDL has two periods. */
    if (!isnan(this->audio_clock)) {
        this->audclk.SetAt(this->audio_clock - (double)(2 * this->audio_hw_buf_size + this->audio_buf_size) / this->audio_tgt.BytesPerSec, this->audio_clock_serial, audio_callback_time / 1000000.0);
        this->extclk.SyncToSlave(&this -> audclk);
    }
    return time;
}

AVPixelFormat FFFmpegMediaTracks::ConvertDeprecatedFormat(AVPixelFormat format)
{
    switch (format)
    {
    case AV_PIX_FMT_YUVJ420P:
        return AV_PIX_FMT_YUV420P;
        break;
    case AV_PIX_FMT_YUVJ422P:
        return AV_PIX_FMT_YUV422P;
        break;
    case AV_PIX_FMT_YUVJ444P:
        return AV_PIX_FMT_YUV444P;
        break;
    case AV_PIX_FMT_YUVJ440P:
        return AV_PIX_FMT_YUV440P;
        break;
    default:
        return format;
        break;
    }
}

void FFFmpegMediaTracks::GetFlags(bool& OutMediaSourceChanged, bool& OutSelectionChanged) const
{
    FScopeLock Lock(&CriticalSection);

    OutMediaSourceChanged = MediaSourceChanged;
    OutSelectionChanged = SelectionChanged;
}

/**
* 与ffplay中不同，添加了rtmp的判断，否则会判断成非实时流，但ffplay中播放时没有问题的，todo: 以后排查吧
*/
int FFFmpegMediaTracks::is_realtime(AVFormatContext* s)
{
    if (!strcmp(s->iformat->name, "rtp")
        || !strcmp(s->iformat->name, "rtsp")
        || !strcmp(s->iformat->name, "sdp")
        )
        return 1;
    if (s->pb && (!strncmp(s->url, "rtp:", 4)
        || !strncmp(s->url, "udp:", 4) || !strncmp(s->url, "rtmp:", 5)
        )
        )
        return 1;
    return 0;
}

//打开指定的视频流
int FFFmpegMediaTracks::stream_component_open(int stream_index)
{
    //unsigned int stream_index_ = (unsigned)stream_index; //进行一步类型转化，否则编译失败
    AVCodecContext* avctx; //codec上下文
    const AVCodec* codec; //codec(解码器)
    const AVDictionaryEntry* t = NULL; //键值对
    int sample_rate; //采样率
    //AVChannelLayout* ch_layout = { 0 };
    AVChannelLayout ch_layout{}; //音频通道格式类型, av_channel_layout_default();
    int ret = 0;
    int lowres = 0; //todo 低分辨率，默认为0
    int stream_lowres = lowres;

    if (stream_index < 0 || stream_index >= (int)ic->nb_streams) //如果流索引小于0或者超过总数量，返回-1
        return -1;

    avctx = avcodec_alloc_context3(NULL); //分配codec上下文对象
    if (!avctx)
        return AVERROR(ENOMEM);

    ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar); //从流中拷贝信息到codec上下文中
    if (ret < 0)
        goto fail;
    avctx->pkt_timebase = ic->streams[stream_index]->time_base;

    codec = avcodec_find_decoder(avctx->codec_id); //查找codec
    //此处删除了ffplay中强制指定编码器的相关代码
    if (!codec) {
        UE_LOG(LogFFmpegMedia, Warning, TEXT("Tracks: %p: No decoder could be found for codec %s"), this, avcodec_get_name(avctx->codec_id));
        ret = AVERROR(EINVAL);
        goto fail;
    }

    avctx->codec_id = codec->id;
    
    if (stream_lowres > codec->max_lowres) { //低分辨率不能超过codec的编码器
        UE_LOG(LogFFmpegMedia, Warning, TEXT("Tracks: %p: The maximum value for lowres supported by the decoder is %d"), this, codec->max_lowres);
        stream_lowres = codec->max_lowres;
    }
    avctx->lowres = stream_lowres;

    static int fast = 0; //todo:非标准化规范的多媒体兼容优化 默认为0
    if (fast)
        avctx->flags2 |= AV_CODEC_FLAG2_FAST;

    AVDictionary* opts = {};
    if (!av_dict_get(opts, "threads", NULL, 0))
        av_dict_set(&opts, "threads", "auto", 0);
    if (stream_lowres)
        av_dict_set_int(&opts, "lowres", stream_lowres, 0);
    if ((ret = avcodec_open2(avctx, codec, &opts)) < 0) { //打开codec
        goto fail;
    }
    t = av_dict_get(opts, "", NULL, AV_DICT_IGNORE_SUFFIX);
    if (t) {
        //av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
        UE_LOG(LogFFmpegMedia, Error, TEXT("Tracks: %p: Option %s not found"), this, t->key);
        ret = AVERROR_OPTION_NOT_FOUND;
        goto fail;
    }

    this->eof = 0;
    ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;
    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        UE_LOG(LogFFmpegMedia, Verbose, TEXT("Tracks %p: Enabled stream[audio] %i"), this, stream_index);
        sample_rate = avctx->sample_rate; //设置音频采样率
        ret = av_channel_layout_copy(&ch_layout, &avctx->ch_layout); //拷贝音频编码格式
        if (ret < 0)
            goto fail;
        //其他变量初始化移动到SelectTrack中
        this->audio_stream = stream_index;
        this->audio_st = ic->streams[stream_index];

        //初始化音频解码器
        ret = this->auddec->Init(avctx, &this->audioq, this->continue_read_thread);
        if (ret < 0)
            goto fail;
        if ((this->ic->iformat->flags & (AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK)) && !this->ic->iformat->read_seek) {
            this->auddec->SetStartPts(this->audio_st->start_time);
            this->auddec->SetStartPtsTb(this->audio_st->time_base);
        }
        //启用音频线程
        if ((ret = auddec->Start([this](void* data) {return audio_thread();}, NULL)) < 0) {
            av_dict_free(&opts);
            return ret;
        }
        UE_LOG(LogFFmpegMedia, Error, TEXT("Tracks: %p: start a audio_thread"), this);
        break;
    case AVMEDIA_TYPE_VIDEO:
        UE_LOG(LogFFmpegMedia, Verbose, TEXT("Tracks %p: Enabled stream[video] %i"), this, stream_index);
        this->video_stream = stream_index;
        this->video_st = ic->streams[stream_index];
        ret = this->viddec->Init(avctx, &this->videoq, this->continue_read_thread);
        if (ret < 0)
            goto fail;
        //启用视频线程
        if ((ret = viddec->Start([this](void* data) {return video_thread();}, NULL)) < 0) {
            goto out;
        }
        this->queue_attachments_req = 1;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        UE_LOG(LogFFmpegMedia, Verbose, TEXT("Tracks %p: Enabled stream[subtitle] %i"), this, stream_index);
        this->subtitle_stream = stream_index;
        this->subtitle_st = ic->streams[stream_index];
        ret = this->subdec->Init(avctx, &this->subtitleq, this->continue_read_thread);
        if (ret < 0)
            goto fail;
        //启用字幕线程
        if ((ret = subdec->Start([this](void* data) {return subtitle_thread();}, NULL)) < 0) {
            goto out;
        }
        break;
    default:
        break;
    }
    goto out;

fail:
    avcodec_free_context(&avctx);
out:
    av_channel_layout_uninit(&ch_layout);
    av_dict_free(&opts);
    return ret;
}

void FFFmpegMediaTracks::stream_component_close(int stream_index)
{
    AVCodecParameters* codecpar;
    int nb_streams = (int)this->ic->nb_streams;
    if (stream_index < 0 || stream_index >= nb_streams)
        return;
    codecpar = this->ic->streams[stream_index]->codecpar;

    switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        this->auddec->Abort(&this->sampq);
        this->auddec->Destroy();
        if (this->swr_ctx) {
            swr_free(&this->swr_ctx);
        }
        if (this->audio_buf1) {
            av_freep(&this->audio_buf1);
        }
        this->audio_buf1_size = 0;
        this->audio_buf = NULL;

        if (this->rdft != NULL) {
            av_rdft_end(this->rdft);
            av_freep(&this->rdft_data);
            this->rdft = NULL;
            this->rdft_bits = 0;
        }
        break;
    case AVMEDIA_TYPE_VIDEO:
        this->viddec->Abort(&this->pictq);
        this->viddec->Destroy();
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        this->subdec->Abort(&this->subpq);
        this->subdec->Destroy();
        break;
    default:
        break;
    }

    ic->streams[stream_index]->discard = AVDISCARD_ALL;
    switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        this->audio_st = NULL;
        this->audio_stream = -1;
        break;
    case AVMEDIA_TYPE_VIDEO:
        this->video_st = NULL;
        this->video_stream = -1;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        this->subtitle_st = NULL;
        this->subtitle_stream = -1;
        break;
    default:
        break;
    }
}

//音频解码线程
int FFFmpegMediaTracks::audio_thread()
{
    AVFrame* frame = av_frame_alloc(); //分配帧对象
    FFmpegFrame* af;
    int got_frame = 0;
    AVRational tb;
    int ret = 0;

    if (!frame) {
        UE_LOG(LogFFmpegMedia, Error, TEXT("Tracks %p: audio_thread exit because alloc frame fail"), this);
        return AVERROR(ENOMEM);
    }
        
    do {
        got_frame = this->auddec->DecodeFrame(frame, NULL); //解码帧
        if (got_frame < 0) {
            av_frame_free(&frame);
            UE_LOG(LogFFmpegMedia, Error, TEXT("Tracks %p: audio_thread exit because decode frame fail"), this);
            return ret;
        }
        if (got_frame) {
            tb = { 1, frame->sample_rate };
            af = this->sampq.PeekWritable();
            if (!af) {
                av_frame_free(&frame);
                UE_LOG(LogFFmpegMedia, Error, TEXT("Tracks %p: audio_thread exit because peek a writable frame fail"), this);
                return ret;
            }
            af->SetPts((frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb));
            af->SetPos(frame->pkt_pos);
            af->SetSerial(this->auddec->GetPktSerial());
            af->SetDuration(av_q2d({ frame->nb_samples, frame->sample_rate }));
            av_frame_move_ref(af->GetFrame(), frame);
            this->sampq.Push();
        }
    } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);

    av_frame_free(&frame);
    UE_LOG(LogFFmpegMedia, Log, TEXT("Tracks %p: audio_thread exit"), this);
    return ret;
}

//视频解码线程
int FFFmpegMediaTracks::video_thread()
{
    AVFrame* frame = av_frame_alloc();
    double pts;
    double duration;
    int ret;
    AVRational tb = this->video_st->time_base;
    AVRational frame_rate = av_guess_frame_rate(this->ic, this->video_st, NULL);

    if (!frame) {
        UE_LOG(LogFFmpegMedia, Error, TEXT("Tracks %p: video_thread exit because alloc frame fail"), this);
        return AVERROR(ENOMEM);
    }

    for (;;) {
        ret = this->get_video_frame(frame);
        if (ret < 0) {
            UE_LOG(LogFFmpegMedia, Error, TEXT("Tracks %p: video_thread exit because decode frame fail"), this);
            av_frame_free(&frame);
            return 0;
        }
        if (!ret)
            continue;

        duration = (frame_rate.num && frame_rate.den ? av_q2d({ frame_rate.den, frame_rate.num }) : 0);
        pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
        ret = queue_picture(frame, pts, duration, frame->pkt_pos, this->viddec->GetPktSerial());
        av_frame_unref(frame);
        if (ret < 0) {
            UE_LOG(LogFFmpegMedia, Error, TEXT("Tracks %p: video_thread exit because picture queue fail"), this);
            av_frame_free(&frame);
            return 0;
        }
    }

    UE_LOG(LogFFmpegMedia, Log, TEXT("Tracks %p: video_thread exit"), this);
    av_frame_free(&frame);
    return 0;
}

int FFFmpegMediaTracks::subtitle_thread()
{
    FFmpegFrame* sp;
    int got_subtitle;
    double pts;

    for (;;) {
        sp = this->subpq.PeekWritable();
        if (!sp) {
            UE_LOG(LogFFmpegMedia, Error, TEXT("Tracks %p: subtitle_thread exit because peek a writable frame fail"), this);
            return 0;
        }

        got_subtitle = this->subdec->DecodeFrame(NULL, &sp->GetSub());
        if (got_subtitle < 0)
            break;
        pts = 0;

        if (got_subtitle && sp->GetSub().format == 0) {
            if (sp->GetSub().pts != AV_NOPTS_VALUE)
                pts = sp->GetSub().pts / (double)AV_TIME_BASE;
            sp->SetPts(pts);
            sp->SetSerial(this->subdec->GetPktSerial());
            sp->SetWidth(this->subdec->GetAvctx()->width);
            sp->SetHeight(this->subdec->GetAvctx()->height);
            sp->SetUploaded(0);

            this->subpq.Push();
        }
        else if (got_subtitle) {
            avsubtitle_free(&sp->GetSub());
        }
    }

    UE_LOG(LogFFmpegMedia, Log, TEXT("Tracks %p: subtitle_thread exit"), this);
    return 0;
}

int FFFmpegMediaTracks::get_video_frame(AVFrame* frame)
{
    int got_picture = this->viddec->DecodeFrame(frame, NULL);

    if (got_picture < 0)
        return -1;

    if (got_picture) {
        double dpts = NAN;

        if (frame->pts != AV_NOPTS_VALUE)
            dpts = av_q2d(this->video_st->time_base) * frame->pts;

        frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(this->ic, this->video_st, frame);
        int framedrop = -1; //todo: drop frames when cpu is too slow
        if (framedrop > 0 || (framedrop && this->get_master_sync_type() != AV_SYNC_VIDEO_MASTER)) {
            if (frame->pts != AV_NOPTS_VALUE) {
                double diff = dpts - this->get_master_clock();
                if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD &&
                    diff - this->frame_last_filter_delay < 0 &&
                    this->viddec->GetPktSerial() == this->vidclk.GetSerial() &&
                    this->videoq.nb_packets) {
                    this->frame_drops_early++;
                    av_frame_unref(frame);
                    got_picture = 0;
                }
            }
        }
    }

    return got_picture;
}

int FFFmpegMediaTracks::get_master_sync_type()
{
    if (this->av_sync_type == AV_SYNC_VIDEO_MASTER) {
        if (this->video_st)
            return AV_SYNC_VIDEO_MASTER;
        else
            return AV_SYNC_AUDIO_MASTER;
    }
    else if (this->av_sync_type == AV_SYNC_AUDIO_MASTER) {
        if (this->audio_st)
            return AV_SYNC_AUDIO_MASTER;
        else
            return AV_SYNC_EXTERNAL_CLOCK;
    }
    else {
        return AV_SYNC_EXTERNAL_CLOCK;
    }
}

double FFFmpegMediaTracks::get_master_clock()
{
    double val;

    switch (this->get_master_sync_type()) {
    case AV_SYNC_VIDEO_MASTER:
        val = this->vidclk.Get();
        break;
    case AV_SYNC_AUDIO_MASTER:
        val = this->audclk.Get();
        break;
    default:
        val = this->extclk.Get();
        break;
    }
    return val;
}

int FFFmpegMediaTracks::queue_picture(AVFrame* src_frame, double pts, double duration, int64_t pos, int serial)
{
    FFmpegFrame* vp = this->pictq.PeekWritable();
    if (!vp)
        return -1;
    
    vp->SetSar(src_frame->sample_aspect_ratio);
    vp->SetUploaded(0);

    vp->SetWidth(src_frame->width);
    vp->SetHeight(src_frame->height);
    vp->SetFormat(src_frame->format);

    vp->SetPts(pts);
    vp->SetDuration(duration);
    vp->SetPos(pos);
    vp->SetSerial(serial);

    av_frame_move_ref(vp->GetFrame(), src_frame);
    this->pictq.Push();
    return 0;
}

/** 单帧播放操作，目前不支持该操作，所以需要保证该方法不会被调用到 */
void FFFmpegMediaTracks::step_to_next_frame()
{
    /* if the stream is paused unpause it, then step */
    if (this->paused)
        this->stream_toggle_pause();
    this->step = 1;
}

void FFFmpegMediaTracks::stream_toggle_pause()
{
    if (this->paused) {
        this->frame_timer += av_gettime_relative() / 1000000.0 - this->vidclk.GetLastUpdated();
        if (this->read_pause_return != AVERROR(ENOSYS)) {
            this->vidclk.SetPaused(0);
        }
        this->vidclk.Set(this->vidclk.Get(), this->vidclk.GetSerial());
    }
    this->extclk.Set(this->extclk.Get(), this->extclk.GetSerial());
    this->paused = this->paused == 1 ? 0 : 1;
    UE_LOG(LogFFmpegMedia, Warning, TEXT("Player %p: stream_toggle_pause this->paused %d"), this, this->paused);
    this->audclk.SetPaused(this->paused);
    this->vidclk.SetPaused(this->paused);
    this->extclk.SetPaused(this->paused);
}

// 不支持字节seek, by_bytes参数永远为0
void FFFmpegMediaTracks::stream_seek(int64_t pos, int64_t rel, int by_bytes)
{
    if (!this->seek_req) { //判断当前是否已经存在seek操作
        this->seek_pos = pos;
        this->seek_rel = rel;
        this->seek_flags &= ~AVSEEK_FLAG_BYTE;
        if (by_bytes)
            this->seek_flags |= AVSEEK_FLAG_BYTE;
        this->seek_req = 1;
        this->continue_read_thread->signal();
    }
}

double FFFmpegMediaTracks::vp_duration(FFmpegFrame* vp, FFmpegFrame* nextvp)
{
    if (vp->GetSerial() == nextvp->GetSerial()) {
        double duration = nextvp->GetPts() - vp->GetPts();
        if (isnan(duration) || duration <= 0 || duration > this->max_frame_duration)
            return vp->GetDuration();
        else
            return duration;
    }
    else {
        return 0.0;
    }
}

double FFFmpegMediaTracks::compute_target_delay(double delay)
{
    double sync_threshold, diff = 0;

    /* update delay to follow master synchronisation source */
    if (this->get_master_sync_type() != AV_SYNC_VIDEO_MASTER) {
        /* if video is slave, we try to correct big delays by
           duplicating or deleting a frame */
        diff = this->vidclk.Get() - this->get_master_clock();

        /* skip or repeat frame. We take into account the
           delay to compute the threshold. I still don't know
           if it is the best guess */
        sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
        if (!isnan(diff) && fabs(diff) < this->max_frame_duration) {
            if (diff <= -sync_threshold)
                delay = FFMAX(0, delay + diff);
            else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD)
                delay = delay + diff;
            else if (diff >= sync_threshold)
                delay = 2 * delay;
        }
    }

    av_log(NULL, AV_LOG_TRACE, "video: delay=%0.3f A-V=%f\n",
        delay, -diff);

    return delay;
}

void FFFmpegMediaTracks::update_video_pts(double pts, int64_t pos, int serial)
{
    /* update current video pts */
    this->vidclk.Set(pts, serial);
    this->extclk.SyncToSlave(&this->vidclk);
}
/** 视频显示 */
void FFFmpegMediaTracks::video_display()
{
   //只有一种显示模式，直接调用
   video_image_display();
}
/** 视频图片显示 */
void FFFmpegMediaTracks::video_image_display()
{
    FFmpegFrame* vp;
    FFmpegFrame* sp = NULL;
    vp = this->pictq.PeekLast(); //读取上一帧(注意此处的帧为将要显示的帧，不是显示过的帧)

    //字幕处理
    if (this->subtitle_st) {
        if (this->subpq.NbRemaining() > 0) {
            sp = this->subpq.Peek();

            if (vp->GetPts() >= sp->GetPts() + ((float)sp->GetSub().start_display_time / 1000)) {
                if (!sp->GetUploaded()) {
                    //uint8_t* pixels[4];
                    //int pitch[4];
                    unsigned int i;
                    if (!sp->width || !sp->height) {
                        sp->width = vp->width;
                        sp->height = vp->height;
                    }
                   /* if (realloc_texture(&is->sub_texture, SDL_PIXELFORMAT_ARGB8888, sp->width, sp->height, SDL_BLENDMODE_BLEND, 1) < 0)
                        return;*/

                    for (i = 0; i < sp->sub.num_rects; i++) {
                        AVSubtitleRect* sub_rect = sp->sub.rects[i];

                        sub_rect->x = av_clip(sub_rect->x, 0, sp->width);
                        sub_rect->y = av_clip(sub_rect->y, 0, sp->height);
                        sub_rect->w = av_clip(sub_rect->w, 0, sp->width - sub_rect->x);
                        sub_rect->h = av_clip(sub_rect->h, 0, sp->height - sub_rect->y);

                       /* is->sub_convert_ctx = sws_getCachedContext(is->sub_convert_ctx,
                            sub_rect->w, sub_rect->h, AV_PIX_FMT_PAL8,
                            sub_rect->w, sub_rect->h, AV_PIX_FMT_BGRA,
                            0, NULL, NULL, NULL);
                        if (!is->sub_convert_ctx) {
                            av_log(NULL, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
                            return;
                        }
                        if (!SDL_LockTexture(is->sub_texture, (SDL_Rect*)sub_rect, (void**)pixels, pitch)) {
                            sws_scale(is->sub_convert_ctx, (const uint8_t* const*)sub_rect->data, sub_rect->linesize,
                                0, sub_rect->h, pixels, pitch);
                            SDL_UnlockTexture(is->sub_texture);
                        }*/
                    }
                    sp->uploaded = 1;
                }
            }
            else
                sp = NULL;
        }
    }

    //判断当前帧是否已经上传
    if (!vp->uploaded) {
        if (upload_texture(vp, vp->frame) < 0) { //上传帧
            return;
        }
        vp->uploaded = 1;
        vp->flip_v = vp->frame->linesize[0] < 0;
    }
}

int FFFmpegMediaTracks::upload_texture(FFmpegFrame* vp, AVFrame* frame)
{
    //目标图像格式, 将帧中的像素统一转化成该格式
    AVPixelFormat targetPixelFormat = AV_PIX_FMT_RGBA;
    int ret = 0;

    //生成转化上下文
    img_convert_ctx = sws_getCachedContext(
        this->img_convert_ctx, //
        frame->width,  //输入图像的宽度
        frame->height, //输入图像的宽度
        ConvertDeprecatedFormat((AVPixelFormat)frame->format), //输入图像的像素格式
        frame->width, //输出图像的宽度
        frame->height, //输出图像的高度
        AV_PIX_FMT_BGRA, //输出图像的像素格式
        SWS_BICUBIC, //选择缩放算法(只有当输入输出图像大小不同时有效), 默认SWS_BICUBIC
        NULL,//输入图像的滤波器信息, 若不需要传NULL
        NULL, //输出图像的滤波器信息, 若不需要传NULL
        NULL); //特定缩放算法需要的参数(? )，默认为NULL

    if (img_convert_ctx != NULL) {

        uint8_t* pixels[4] = {0};
        int pitch[4] = { 0,0,0,0 };
        int size = av_image_get_buffer_size(AV_PIX_FMT_RGBA, frame->width, frame->height, 1);

        this->ImgaeCopyDataBuffer.Reset();
        this->ImgaeCopyDataBuffer.AddUninitialized(size);

        av_image_fill_linesizes(pitch, AV_PIX_FMT_BGRA, frame->width); //填充每个颜色通道的行字节数
        av_image_fill_pointers(pixels, AV_PIX_FMT_BGRA, frame->height, ImgaeCopyDataBuffer.GetData(), pitch);
        
        //视频像素格式和分辨率的转换
        sws_scale(
            img_convert_ctx,
            (const uint8_t* const*)frame->data, //输入图像的每个颜色通道的数据指针
            frame->linesize, //输入图像的每个颜色通道的跨度,也就是每个通道的行字节数
            0, //起始位置
            frame->height, //处理多少行   0-frame->height 表示一次性处理完整个图像
            pixels, ///输出图像的每个颜色通道的数据指针
            pitch); ///输入图像的每个颜色通道的行字节数

        {
            FScopeLock Lock(&CriticalSection);
            //从纹理样本池中获取一个共享对象
            const TSharedRef<FFFmpegMediaTextureSample, ESPMode::ThreadSafe> TextureSample
                = VideoSamplePool->AcquireShared();
            //根据帧初始化该对象
            FIntPoint Dim = { frame->width, frame->height };
            FTimespan time = FTimespan::FromSeconds(0);
            if (!isnan(vp->GetPts())) {
                time = FTimespan::FromSeconds(vp->GetPts());
            }
            FTimespan duration = FTimespan::FromSeconds(vp->GetDuration());
            if (TextureSample->Initialize(
                ImgaeCopyDataBuffer.GetData(),
                ImgaeCopyDataBuffer.Num(),
                Dim,
                pitch[0],
                time + duration,
                duration))
            {
                //将样本对象放入样本队列中
                VideoSampleQueue.Enqueue(TextureSample);
            }
        }
        return ret;
    }
    else {
        UE_LOG(LogFFmpegMedia, Error, TEXT("Cannot initialize the conversion context"));
        ret = -1;
        return ret;
    }
}

int FFFmpegMediaTracks::audio_decode_frame(FTimespan& time, FTimespan& duration)
{
    int data_size, resampled_data_size;
    av_unused double audio_clock0;
    int wanted_nb_samples;
    FFmpegFrame* af;

    if (this->paused)
        return -1;
    if (CurrentState == EMediaState::Paused || CurrentState == EMediaState::Stopped)
        return -1;

    do {
        af = this->sampq.PeekReadable();
        if (!af)
            return -1;
        this->sampq.Next();
    } while (af->serial != this->audioq.serial);
    data_size = av_samples_get_buffer_size(NULL, af->frame->ch_layout.nb_channels,
        af->frame->nb_samples,
        (AVSampleFormat)af->frame->format, 1);

    wanted_nb_samples = synchronize_audio(af->frame->nb_samples);

    if (af->frame->format != this->audio_src.Format ||
        av_channel_layout_compare(&af->frame->ch_layout, &this->audio_src.ChannelLayout) ||
        af->frame->sample_rate != this->audio_src.SampleRate ||
        (wanted_nb_samples != af->frame->nb_samples && !this->swr_ctx)) {
        swr_free(&this->swr_ctx);
        swr_alloc_set_opts2(&this->swr_ctx,
            &this->audio_tgt.ChannelLayout, this->audio_tgt.Format, this->audio_tgt.SampleRate,
            &af->frame->ch_layout, (AVSampleFormat)af->frame->format, af->frame->sample_rate,
            0, NULL);
        if (!this->swr_ctx || swr_init(this->swr_ctx) < 0) {
            UE_LOG(LogFFmpegMedia, Error, TEXT("Tracks: %p: Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!"), this,
                af->frame->sample_rate, av_get_sample_fmt_name((AVSampleFormat)af->frame->format), af->frame->ch_layout.nb_channels,
                this->audio_tgt.SampleRate, av_get_sample_fmt_name(this->audio_tgt.Format), this->audio_tgt.ChannelLayout.nb_channels);
            swr_free(&this->swr_ctx);
            return -1;
        }
        if (av_channel_layout_copy(&this->audio_src.ChannelLayout, &af->frame->ch_layout) < 0)
            return -1;
        this->audio_src.SampleRate = af->frame->sample_rate;
        this->audio_src.Format = (AVSampleFormat)af->frame->format;
    }

    if (this->swr_ctx) {
        const uint8_t** in = (const uint8_t**)af->frame->extended_data;
        uint8_t** out = &this->audio_buf1;
        int out_count = (int64_t)wanted_nb_samples * this->audio_tgt.SampleRate / af->frame->sample_rate + 256;
        int out_size = av_samples_get_buffer_size(NULL, this->audio_tgt.ChannelLayout.nb_channels, out_count, this->audio_tgt.Format, 0);
        int len2;
        if (out_size < 0) {
            UE_LOG(LogFFmpegMedia, Error, TEXT("Tracks: %p: av_samples_get_buffer_size() failed"), this);
            return -1;
        }
        if (wanted_nb_samples != af->frame->nb_samples) {
            if (swr_set_compensation(this->swr_ctx, (wanted_nb_samples - af->frame->nb_samples) * this->audio_tgt.SampleRate / af->frame->sample_rate,
                wanted_nb_samples * this->audio_tgt.SampleRate / af->frame->sample_rate) < 0) {
                UE_LOG(LogFFmpegMedia, Error, TEXT("Tracks: %p: swr_set_compensation() failed"), this);
                return -1;
            }
        }
        av_fast_malloc(&this->audio_buf1, &this->audio_buf1_size, out_size); //该方法如果值错误，将会导致程序直接崩溃
        if (!this->audio_buf1)
            return AVERROR(ENOMEM);
        len2 = swr_convert(this->swr_ctx, out, out_count, in, af->frame->nb_samples);
        if (len2 < 0) {
            UE_LOG(LogFFmpegMedia, Error, TEXT("Tracks: %p: swr_convert() failed"), this);
            return -1;
        }
        if (len2 == out_count) {
            UE_LOG(LogFFmpegMedia, Warning, TEXT("Tracks: %p: audio buffer is probably too small"), this);
            if (swr_init(this->swr_ctx) < 0)
                swr_free(&this->swr_ctx);
        }
        this->audio_buf = this->audio_buf1;
        resampled_data_size = len2 * this->audio_tgt.ChannelLayout.nb_channels * av_get_bytes_per_sample(this->audio_tgt.Format);
    }
    else {
        this->audio_buf = af->frame->data[0];
        resampled_data_size = data_size;
    }

    audio_clock0 = this->audio_clock;
    /* update the audio clock with the pts */
    if (!isnan(af->pts))
        this->audio_clock = af->pts + (double)af->frame->nb_samples / af->frame->sample_rate;
    else
        this->audio_clock = NAN;
    this->audio_clock_serial = af->serial;

    time = FTimespan::FromSeconds(this->audio_clock); 
    duration = FTimespan::FromSeconds(af->GetDuration());
    return resampled_data_size;
}
//音视频同步
int FFFmpegMediaTracks::synchronize_audio(int nb_samples)
{
    int wanted_nb_samples = nb_samples;

    /* if not master, then we try to remove or add samples to correct the clock */
    if (this->get_master_sync_type() != AV_SYNC_AUDIO_MASTER) {
        double diff, avg_diff;
        int min_nb_samples, max_nb_samples;

        diff = this->audclk.Get() - this->get_master_clock();

        if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
            this->audio_diff_cum = diff + this->audio_diff_avg_coef * this->audio_diff_cum;
            if (this->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
                /* not enough measures to have a correct estimate */
                this->audio_diff_avg_count++;
            }
            else {
                /* estimate the A-V difference */
                avg_diff = this->audio_diff_cum * (1.0 - this->audio_diff_avg_coef);

                if (fabs(avg_diff) >= this->audio_diff_threshold) {
                    wanted_nb_samples = nb_samples + (int)(diff * this->audio_src.SampleRate);
                    min_nb_samples = ((nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    max_nb_samples = ((nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    wanted_nb_samples = av_clip(wanted_nb_samples, min_nb_samples, max_nb_samples);
                }
                av_log(NULL, AV_LOG_TRACE, "diff=%f adiff=%f sample_diff=%d apts=%0.3f %f\n",
                    diff, avg_diff, wanted_nb_samples - nb_samples,
                    this->audio_clock, this->audio_diff_threshold);
            }
        }
        else {
            /* too big difference : may be initial PTS errors, so
               reset A-V filter */
            this->audio_diff_avg_count = 0;
            this->audio_diff_cum = 0;
        }
    }

    return wanted_nb_samples;
}

int FFFmpegMediaTracks::stream_has_enough_packets(AVStream* st, int stream_id, FFmpegPacketQueue* queue)
{
    return stream_id < 0 ||
        queue->abort_request ||
        (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
        queue->nb_packets > MIN_FRAMES && (!queue->duration || av_q2d(st->time_base) * queue->duration > 1.0);
}


bool FFFmpegMediaTracks::FetchAudio(TRange<FTimespan> TimeRange, TSharedPtr<IMediaAudioSample, ESPMode::ThreadSafe>& OutSample)
{
    TSharedPtr<IMediaAudioSample, ESPMode::ThreadSafe> Sample;
    if (!AudioSampleQueue.Peek(Sample))
    {
        UE_LOG(LogFFmpegMedia, Warning, TEXT("Tracks[FetchAudio]: %p , peek a sample failed"), this);
        return false;
    }

    const FTimespan SampleTime = Sample->GetTime().Time;
    if (!TimeRange.Overlaps(TRange<FTimespan>(SampleTime, SampleTime + Sample->GetDuration())))
    {
        UE_LOG(LogFFmpegMedia, Warning, TEXT("Tracks[FetchAudio]: %p, sample not allow time range"), this);
        return false;
    }

    if (!AudioSampleQueue.Dequeue(Sample))
    {
        UE_LOG(LogFFmpegMedia, Warning, TEXT("Tracks[FetchAudio]: %p, delete sample form queue failed"), this);
        return false;
    }

    OutSample = Sample;
    return true;
}

bool FFFmpegMediaTracks::FetchCaption(TRange<FTimespan> TimeRange, TSharedPtr<IMediaOverlaySample, ESPMode::ThreadSafe>& OutSample)
{
    return false;
}

bool FFFmpegMediaTracks::FetchMetadata(TRange<FTimespan> TimeRange, TSharedPtr<IMediaBinarySample, ESPMode::ThreadSafe>& OutSample)
{
    return false;
}

void FFFmpegMediaTracks::FlushSamples()
{
    UE_LOG(LogFFmpegMedia, VeryVerbose, TEXT("FFmpegMediaTracks::FlushSamples"));
    AudioSampleQueue.RequestFlush();
    CaptionSampleQueue.RequestFlush();
    MetadataSampleQueue.RequestFlush();
    VideoSampleQueue.RequestFlush();
}

/** 当音频不可用时，会读取视频时间，需要保证视频第一个样本时间有效，否则不会去读取样本，造成死锁 */
bool FFFmpegMediaTracks::PeekVideoSampleTime(FMediaTimeStamp& TimeStamp)
{
    TSharedPtr<IMediaTextureSample, ESPMode::ThreadSafe> Sample;
    if (!VideoSampleQueue.Peek(Sample))
    {
        return false;
    }
  
    TimeStamp = FMediaTimeStamp(Sample->GetTime());
    return true;
}

IMediaSamples::EFetchBestSampleResult FFFmpegMediaTracks::FetchBestVideoSampleForTimeRange(const TRange<FMediaTimeStamp>& TimeRange, TSharedPtr<IMediaTextureSample, ESPMode::ThreadSafe>& OutSample, bool bReverse)
{
    //VeryVerbose
    // Don't return any samples if we are stopped. We could be prerolling.
    if (CurrentState == EMediaState::Stopped)
    {
        return IMediaSamples::EFetchBestSampleResult::NoSample;
    }

    IMediaSamples::EFetchBestSampleResult Result = IMediaSamples::EFetchBestSampleResult::NoSample;
    FTimespan TimeRangeLow = TimeRange.GetLowerBoundValue().Time;
    FTimespan TimeRangeHigh = TimeRange.GetUpperBoundValue().Time;

    
    if (this->realtime) {
        while (true) {
            TSharedPtr<IMediaTextureSample, ESPMode::ThreadSafe> Sample;
            if (VideoSampleQueue.Peek(Sample))
            {
                FTimespan SampleStartTime = Sample->GetTime().Time;
                FTimespan SampleEndTime = SampleStartTime + Sample->GetDuration();//  *10;
                UE_LOG(LogFFmpegMedia, VeryVerbose, TEXT("FetchBestVideoSampleForTimeRange looking at sample %f:%d %f"),
                    SampleStartTime.GetTotalSeconds(), Sample->GetTime().SequenceIndex, SampleEndTime.GetTotalSeconds());

                //// Yes. Use this sample.
                if (!VideoSampleQueue.Dequeue(OutSample))
                {
                    /*Result = IMediaSamples::EFetchBestSampleResult::Ok;
                    UE_LOG(LogFFmpegMedia, VeryVerbose, TEXT("FetchBestVideoSampleForTimeRange got sample."));*/
                    break;
                }
                OutSample = Sample;
                Result = IMediaSamples::EFetchBestSampleResult::Ok;
                break;
            }
            else
            {
                // No samples available.
                break;
            }
        }
        return Result;
    }

   
    // Account for loop wraparound.
    if (TimeRangeHigh < TimeRangeLow)
    {
        TimeRangeHigh += Duration;
    }
    TRange<FTimespan> TimeRangeTime(TimeRangeLow, TimeRangeHigh);
    FTimespan LoopDiff = Duration * 0.5f;
    float CurrentOverlap = 0.0f;
    
    UE_LOG(LogFFmpegMedia, VeryVerbose, TEXT("FetchBestVideoSampleForTimeRange %f:%d %f:%d seek:%f"),
        TimeRangeLow.GetTotalSeconds(), 
        TimeRange.GetLowerBoundValue().SequenceIndex, 
        TimeRangeHigh.GetTotalSeconds(), 
        TimeRange.GetUpperBoundValue().SequenceIndex,
        SeekTimeOptional.IsSet() ? SeekTimeOptional->GetTotalSeconds() : -1.0f);

    // Loop over our samples.
    while (true)
    {
        // Is there a sample available?
        TSharedPtr<IMediaTextureSample, ESPMode::ThreadSafe> Sample;
        if (VideoSampleQueue.Peek(Sample))
        {
            FTimespan SampleStartTime = Sample->GetTime().Time;
            FTimespan SampleEndTime = SampleStartTime + Sample->GetDuration();//  *10;
            UE_LOG(LogFFmpegMedia, VeryVerbose, TEXT("FetchBestVideoSampleForTimeRange looking at sample %f:%d %f"),
                SampleStartTime.GetTotalSeconds(), Sample->GetTime().SequenceIndex, SampleEndTime.GetTotalSeconds());


            // Are we waiting for the sample from a seek?
            if (SeekTimeOptional.IsSet())
            {
                // Are we past the seek time?
                if (TimeRangeTime.Contains(SeekTimeOptional.GetValue()) == false)
                {
                    SeekTimeOptional.Reset();
                }
                else
                {
                    // Is this our seek sample?
                    FTimespan SeekTime = SeekTimeOptional.GetValue();
                    double SeekTimeSeconds = SeekTime.GetTotalSeconds();
                    if ((FMath::IsNearlyEqual(SeekTimeSeconds, SampleStartTime.GetTotalSeconds(), 0.001)) ||
                        ((SeekTime >= SampleStartTime) && (SeekTime < SampleEndTime)))
                    {
                        // Yes this is what we have been waiting for.
                        // Reset the seek time so its no longer used.
                        SeekTimeOptional.Reset();
                    }
                    else
                    {
                        // This is not the sample we want, its old.
                        VideoSampleQueue.Pop();
                        continue;
                    }
                }
            }


            // Are we already past this sample?
            if (SampleEndTime < TimeRangeLow)//样本结束时间小于请求开始时间，
            {
                // If there is a large gap to this sample, then its probably because it looped,
                // so we aren't really past it.
                FTimespan Diff = TimeRangeLow - SampleEndTime;
                if (Diff > LoopDiff)
                {
                    // Adjust sample times so they are in the same "space" as the time range.
                    SampleStartTime += Duration;
                    SampleEndTime += Duration;
                    UE_LOG(LogFFmpegMedia, VeryVerbose, TEXT("FetchBestVideoSampleForTimeRange sample loop %f %f"),
                        SampleStartTime.GetTotalSeconds(), SampleEndTime.GetTotalSeconds());
                }
                else
                {
                    // Try next sample.
                    VideoSampleQueue.Pop();
                    continue;
                }
            }

            {
                // Did we already pass this sample,
                // and the sample is at the end of the video and we just looped around?
                FTimespan Diff = SampleEndTime - TimeRangeLow;
                if (Diff > LoopDiff)
                {
                    VideoSampleQueue.Pop();
                    continue;
                }


                // Is this sample before the end of the requested time range?
                if (SampleStartTime < TimeRangeHigh)
                {
                    // Yes.
                    // Does this sample have the largest overlap so far?
                    TRange<FTimespan> SampleRange(SampleStartTime, SampleEndTime);
                    TRange<FTimespan> OverlapRange = TRange<FTimespan>::Intersection(SampleRange, TimeRangeTime);
                    FTimespan OverlapTimespan = OverlapRange.Size<FTimespan>();
                    float Overlap = OverlapTimespan.GetTotalSeconds();
                    if (CurrentOverlap <= Overlap)
                    {
                        // Yes. Use this sample.
                        if (VideoSampleQueue.Dequeue(OutSample))
                        {
                            Result = IMediaSamples::EFetchBestSampleResult::Ok;
                            CurrentOverlap = Overlap;
                            LastFetchVideoTime = Sample->GetTime().Time.GetTotalSeconds();
                            UE_LOG(LogFFmpegMedia, Log, TEXT("FetchBestVideoSampleForTimeRange %f"), Sample->GetTime().Time.GetTotalSeconds());
                            UE_LOG(LogFFmpegMedia, VeryVerbose, TEXT("FetchBestVideoSampleForTimeRange got sample."));
                        }
                    }
                    else
                    {
                        // No need to continue.
                        // This sample is overlapping our end point.
                        break;
                    }
                }
                else
                {
                    // Sample is not before the end of the requested time range.
                    // We are done for now.
                    break;
                }
            }
        }
        else
        {
            // No samples available.
            break;
        }
    }

    return Result;
}

bool FFFmpegMediaTracks::GetAudioTrackFormat(int32 TrackIndex, int32 FormatIndex, FMediaAudioTrackFormat& OutFormat) const
{

    FScopeLock Lock(&CriticalSection);

    if (AudioTracks.IsValidIndex(TrackIndex))
    {
        const FTrack& Track = AudioTracks[TrackIndex];
        const FFormat* Format = &Track.Format;
        OutFormat.BitsPerSample = Format->Audio.FrameSize * 8;
        OutFormat.NumChannels = Format->Audio.NumChannels;
        OutFormat.SampleRate = Format->Audio.SampleRate;
        OutFormat.TypeName = Format->TypeName;
        return true;
    }
    else {
        return false;
    }
}

int32 FFFmpegMediaTracks::GetNumTracks(EMediaTrackType TrackType) const
{
    FScopeLock Lock(&CriticalSection);

    switch (TrackType)
    {
    case EMediaTrackType::Audio: //音频轨道
        return AudioTracks.Num();
    case EMediaTrackType::Caption: //闭合字幕轨道
        return CaptionTracks.Num();
    case EMediaTrackType::Metadata: // todo
        return 0;
    //case EMediaTrackType::Script: 
    //    return 4;
    //case EMediaTrackType::Text: //文本轨道
    //    return 5;
    //case EMediaTrackType::Subtitle:  //字幕轨道
    //    return 6;
    case EMediaTrackType::Video: //视频轨道
        return VideoTracks.Num();
    default:
        break; // unsupported track type
    }
    return 0;
}

int32 FFFmpegMediaTracks::GetNumTrackFormats(EMediaTrackType TrackType, int32 TrackIndex) const
{

    FScopeLock Lock(&CriticalSection);

    switch (TrackType)
    {
    case EMediaTrackType::Audio:
        if (AudioTracks.IsValidIndex(TrackIndex))
        {
            return 1;
        }

    case EMediaTrackType::Metadata:
       /* if (MetadataTracks.IsValidIndex(TrackIndex))
        {
            return 1;
        }*/
        return 1;

    case EMediaTrackType::Caption:
        if (CaptionTracks.IsValidIndex(TrackIndex))
        {
            return 1;
        }

    case EMediaTrackType::Video:
        if (VideoTracks.IsValidIndex(TrackIndex))
        {
            //return VideoTracks[TrackIndex].Formats.Num();
            return 1;
        }
    default:
        break; // unsupported track type
    }

    return 0;
}

int32 FFFmpegMediaTracks::GetSelectedTrack(EMediaTrackType TrackType) const
{
    switch (TrackType)
    {
    case EMediaTrackType::Audio:
        return SelectedAudioTrack;

    case EMediaTrackType::Caption:
        return SelectedCaptionTrack;

    //case EMediaTrackType::Metadata:
        //return SelectedMetadataTrack;
    //    break;
    case EMediaTrackType::Video:
        return SelectedVideoTrack;
    default:
        break; // unsupported track type
    }

    return INDEX_NONE;
}

FText FFFmpegMediaTracks::GetTrackDisplayName(EMediaTrackType TrackType, int32 TrackIndex) const
{
    FScopeLock Lock(&CriticalSection);

    switch (TrackType)
    {
    case EMediaTrackType::Audio:
        if (AudioTracks.IsValidIndex(TrackIndex))
        {
            return AudioTracks[TrackIndex].DisplayName;
        }
        break;

    //case EMediaTrackType::Metadata:
    //    /*if (MetadataTracks.IsValidIndex(TrackIndex))
    //    {
    //        return MetadataTracks[TrackIndex].DisplayName;
    //    }*/
    //    break;

    case EMediaTrackType::Caption:
        if (CaptionTracks.IsValidIndex(TrackIndex))
        {
            return CaptionTracks[TrackIndex].DisplayName;
        }
        break;

    case EMediaTrackType::Video:
        if (VideoTracks.IsValidIndex(TrackIndex))
        {
            return VideoTracks[TrackIndex].DisplayName;
        }
        break;

    default:
        break; // unsupported track type
    }

    return FText::GetEmpty();
}

int32 FFFmpegMediaTracks::GetTrackFormat(EMediaTrackType TrackType, int32 TrackIndex) const
{
    FScopeLock Lock(&CriticalSection);

    switch (TrackType)
    {
    case EMediaTrackType::Audio:
        if (AudioTracks.IsValidIndex(TrackIndex))
        {
            if (&AudioTracks[TrackIndex] != nullptr ) {
                return 0;
            }
        }
        break;
    case EMediaTrackType::Metadata:
      /*  if (MetadataTracks.IsValidIndex(TrackIndex))
        {
            return &MetadataTracks[TrackIndex];
        }*/
        break;
    case EMediaTrackType::Caption:
        if (CaptionTracks.IsValidIndex(TrackIndex))
        {
            if (&CaptionTracks[TrackIndex] != nullptr) {
                return 0;
            }
        }
        break;
    case EMediaTrackType::Video:
        if (VideoTracks.IsValidIndex(TrackIndex))
        {
            if (&VideoTracks[TrackIndex] != nullptr) {
                return 0;
            }
        }

    default:
        break; // unsupported track type
    }

    return INDEX_NONE;

}

FString FFFmpegMediaTracks::GetTrackLanguage(EMediaTrackType TrackType, int32 TrackIndex) const
{
    FScopeLock Lock(&CriticalSection);

    switch (TrackType)
    {
    case EMediaTrackType::Audio:
        if (AudioTracks.IsValidIndex(TrackIndex))
        {
            return AudioTracks[TrackIndex].Language;
        }
        break;

    case EMediaTrackType::Metadata:
       /* if (MetadataTracks.IsValidIndex(TrackIndex))
        {
            return MetadataTracks[TrackIndex].Language;
        }*/
        break;

    case EMediaTrackType::Caption:
        if (CaptionTracks.IsValidIndex(TrackIndex))
        {
            return CaptionTracks[TrackIndex].Language;
        }
        break;

    case EMediaTrackType::Video:
        if (VideoTracks.IsValidIndex(TrackIndex))
        {
            return VideoTracks[TrackIndex].Language;
        }
        break;

    default:
        break; // unsupported track type
    }

    return FString();
}

FString FFFmpegMediaTracks::GetTrackName(EMediaTrackType TrackType, int32 TrackIndex) const
{
    FScopeLock Lock(&CriticalSection);

    switch (TrackType)
    {
    case EMediaTrackType::Audio:
        if (AudioTracks.IsValidIndex(TrackIndex))
        {
            return AudioTracks[TrackIndex].Name;
        }
        break;

    case EMediaTrackType::Metadata:
       /* if (MetadataTracks.IsValidIndex(TrackIndex))
        {
            return MetadataTracks[TrackIndex].Name;
        }*/
        break;

    case EMediaTrackType::Caption:
        if (CaptionTracks.IsValidIndex(TrackIndex))
        {
            return CaptionTracks[TrackIndex].Name;
        }
        break;

    case EMediaTrackType::Video:
        if (VideoTracks.IsValidIndex(TrackIndex))
        {
            return VideoTracks[TrackIndex].Name;
        }
        break;

    default:
        break; // unsupported track type
    }

    return FString();
}

bool FFFmpegMediaTracks::GetVideoTrackFormat(int32 TrackIndex, int32 FormatIndex, FMediaVideoTrackFormat& OutFormat) const
{
    FScopeLock Lock(&CriticalSection);

    if (VideoTracks.IsValidIndex(TrackIndex))
    {
        const FTrack& Track = VideoTracks[TrackIndex];
        const FFormat* Format = &Track.Format;
        OutFormat.Dim = Format->Video.OutputDim;
        OutFormat.FrameRate = Format->Video.FrameRate;
        OutFormat.FrameRates = TRange<float>(Format->Video.FrameRate);
        OutFormat.TypeName = Format->TypeName;
        return true;
    }
    else {
        return false;
    }
}

bool FFFmpegMediaTracks::SelectTrack(EMediaTrackType TrackType, int32 TrackIndex)
{
    //判断媒体是否打开，一定要判断
    if (!this->ic) {
        return false;
    }

    UE_LOG(LogFFmpegMedia, Verbose, TEXT("Tracks %p: Selecting %s track %i"), this, *MediaUtils::TrackTypeToString(TrackType), TrackIndex);
    FScopeLock Lock(&CriticalSection);

    //当前选择通道
    int32* SelectedTrack = nullptr;
    //与当前选择通道类型相同的所有通道
    TArray<FTrack>* Tracks = nullptr;

    switch (TrackType)
    {
    case EMediaTrackType::Audio:
        SelectedTrack = &SelectedAudioTrack;
        Tracks = &AudioTracks;
        break;
    case EMediaTrackType::Caption:
        SelectedTrack = &SelectedCaptionTrack;
        Tracks = &CaptionTracks;
        break;
    //case EMediaTrackType::Metadata:
        /*SelectedTrack = &SelectedMetadataTrack;
        Tracks = &MetadataTracks;*/
        //break;
    case EMediaTrackType::Video:
        SelectedTrack = &SelectedVideoTrack;
        Tracks = &VideoTracks;
        break;
    default:
        return false; // unsupported track type
    }

    check(SelectedTrack != nullptr);
    check(Tracks != nullptr);

    //如果当前要选择的索引与已经选择的索引一致，则表示已经选择该轨道
    if (TrackIndex == *SelectedTrack)
    {
        UE_LOG(LogFFmpegMedia, Log, TEXT("Tracks %p: Already selected %s track %i"), this, *MediaUtils::TrackTypeToString(TrackType), TrackIndex);
        return true; // already selected
    }

    //如果当前要选择的索引为空或者为不可用轨道索引，直接返回
    if ((TrackIndex != INDEX_NONE) && !Tracks->IsValidIndex(TrackIndex))
    {
        UE_LOG(LogFFmpegMedia, Warning, TEXT("Tracks %p: Selecting %s track %i fail"), this, *MediaUtils::TrackTypeToString(TrackType), TrackIndex);
        return false; // invalid track
    }


    // select stream for new track 选择新的轨道
    if (TrackIndex != INDEX_NONE) { //如果要选择的轨道不为空
        const int StreamIndex = (*Tracks)[TrackIndex].StreamIndex;

        //如果轨道不是音频 或者 (是音频且启用音轨)，即关闭音轨的时候跳过音轨
        if (TrackType != EMediaTrackType::Audio || (TrackType == EMediaTrackType::Audio)) {
            this->stream_component_open(StreamIndex);
            this->currentOpenStreamNumber++;
            UE_LOG(LogFFmpegMedia, Verbose, TEXT("Tracks %p: Enabled stream %i"), this, StreamIndex);
        }

        *SelectedTrack = TrackIndex;
        SelectionChanged = true;

        if (TrackType == EMediaTrackType::Video) {
            //开启显示线程
            if (!displayRunning) {
                displayRunning = true;
                displayThread = LambdaFunctionRunnable::RunThreaded("DisplayThread", [this]() {
                    DisplayThread();
                    });
                UE_LOG(LogFFmpegMedia, Error, TEXT("Player %p: Start DisplayThread  success"), this);
            }
        }
        else if (TrackType == EMediaTrackType::Audio) {

            //在此处设置源音频格式和目标格式
            //添加轨道式AV_SAMPLE_FMT_S16已经固定了
            audio_src = (*Tracks)[TrackIndex].Format.Audio;
            audio_tgt = audio_src;
            this->audio_hw_buf_size = this->audio_src.HardwareSize;
            this->audio_buf_size = 0;
            /* init averaging filter */
            this->audio_diff_avg_coef = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
            this->audio_diff_avg_count = 0;
            /* since we do not have a precise anough audio FIFO fullness,
               we correct audio sync only if larger than this threshold */
            this->audio_diff_threshold = (double)(this->audio_tgt.HardwareSize) / this->audio_tgt.BytesPerSec;

            audioRunning = true;
            audioRenderThread = LambdaFunctionRunnable::RunThreaded("AudioRenderThread", [this]() {
                AudioRenderThread();
                });
            DeferredEvents.Enqueue(EMediaEvent::Internal_VideoSamplesUnavailable); //发送事件，
            UE_LOG(LogFFmpegMedia, Error, TEXT("Player %p: Start AudioRenderThread success"), this);
        }
    }

   return true;
}

bool FFFmpegMediaTracks::SetTrackFormat(EMediaTrackType TrackType, int32 TrackIndex, int32 FormatIndex)
{
    UE_LOG(LogFFmpegMedia, Verbose, TEXT("Tracks %p: Setting format on %s track %i to %i"), this, *MediaUtils::TrackTypeToString(TrackType), TrackIndex, FormatIndex);

    FScopeLock Lock(&CriticalSection);

    TArray<FTrack>* Tracks = nullptr;

    switch (TrackType)
    {
    case EMediaTrackType::Audio:
        Tracks = &AudioTracks;
        break;

    case EMediaTrackType::Caption:
        Tracks = &CaptionTracks;
        break;

    case EMediaTrackType::Metadata:
      // Tracks = &MetadataTracks;
        break;

    case EMediaTrackType::Video:
        Tracks = &VideoTracks;
        break;

    default:
        return false; // unsupported track type
    };

    check(Tracks != nullptr);

    if (!Tracks->IsValidIndex(TrackIndex))
    {
        return false; // invalid track index
    }

    return true;
}

bool FFFmpegMediaTracks::SetVideoTrackFrameRate(int32 TrackIndex, int32 FormatIndex, float FrameRate)
{
    UE_LOG(LogFFmpegMedia, Verbose, TEXT("Tracks %p: Setting frame rate on format %i of video track %i to %f"), this, FormatIndex, TrackIndex, FrameRate);

    FScopeLock Lock(&CriticalSection);

    if (VideoTracks.IsValidIndex(TrackIndex))
    {
        const FTrack& Track = VideoTracks[TrackIndex];
        const FFormat* Format = &Track.Format;
        if (Format->Video.FrameRate == FrameRate)
        {
            return true; // frame rate already set
        }
    }
    return false;
}

bool FFFmpegMediaTracks::CanControl(EMediaControl Control) const
{
   // UE_LOG(LogFFmpegMedia, Warning, TEXT("Tracks: %p: CanControl"), this);
    
    if (Control == EMediaControl::Pause)
    {
        //播放时，可以暂停
        return (CurrentState == EMediaState::Playing);
    }

    if (Control == EMediaControl::Resume)
    {
        return (CurrentState != EMediaState::Playing);
    }

    if ((Control == EMediaControl::Scrub) || (Control == EMediaControl::Seek))
    {
        //支持跳转操作
        return true;
    }
    
    return false;
}

FTimespan FFFmpegMediaTracks::GetDuration() const
{
    //在read_thread中读取
    return this->Duration;
}

float FFFmpegMediaTracks::GetRate() const
{
    return this->CurrentRate;
}

EMediaState FFFmpegMediaTracks::GetState() const
{
    return this->CurrentState;
}

EMediaStatus FFFmpegMediaTracks::GetStatus() const
{
    //todo播放器状态，先默认返回None
    return EMediaStatus::None;
}

TRangeSet<float> FFFmpegMediaTracks::GetSupportedRates(EMediaRateThinning Thinning) const
{
    if (Thinning == EMediaRateThinning::Unthinned)
    {
        TRangeSet<float> Result;
        //注意是半开半闭区间
        Result.Add(TRange<float>(0.0f, 2.0f));
        return Result;
    }
    else
    {
        TRangeSet<float> Result;
        //注意是半开半闭区间
        Result.Add(TRange<float>(0.0f, 2.0f));
        return Result;
    }
}

FTimespan FFFmpegMediaTracks::GetTime() const
{
    return this->CurrentTime;
}

bool FFFmpegMediaTracks::IsLooping() const
{
    return this->ShouldLoop;
}

bool FFFmpegMediaTracks::Seek(const FTimespan& Time)
{
    if ((Time < FTimespan::Zero()) || (Time > Duration))
    {
        UE_LOG(LogFFmpegMedia, Verbose, TEXT("Tracks %p: Invalid seek time %s (media duration is %s)"), this, *Time.ToString(), *Duration.ToString());
        return false;
    }

    //此处判断
    if (this->seek_req) {
        UE_LOG(LogFFmpegMedia, Warning, TEXT("Tracks %p: is seeking ..."), this);
        return false;
    }
    int64_t pos = Time.GetTicks() / 10; //需要跳转到位置
    DeferredEvents.Enqueue(EMediaEvent::PlaybackEndReached);
    this->stream_seek(pos, 0, 0);
    SeekTimeOptional = Time;
    return true;
}

bool FFFmpegMediaTracks::SetLooping(bool Looping)
{
    this->ShouldLoop = Looping;
    return true;
}

/**
* 设置播放速率
* 0, 停止播放
* 1， 播放
* 参考ffplay stream_toggle_pause
*/
bool FFFmpegMediaTracks::SetRate(float Rate)
{
    this->CurrentRate = Rate; //设置播放速率

    if (FMath::IsNearlyZero(Rate)) //接近于0，停止播放
    {
        CurrentState = EMediaState::Paused;
        DeferredEvents.Enqueue(EMediaEvent::PlaybackSuspended);

        if (!this->paused) {//如果未暂停，则执行以下操作
            this->extclk.Set(this->extclk.Get(), this->extclk.GetSerial());
            UE_LOG(LogFFmpegMedia, Warning, TEXT("Player %p: SetRate =0 this->paused %d"), this, 1);
            this->paused = 1;
        }
    }
    else //播放
    {
        CurrentState = EMediaState::Playing;
        DeferredEvents.Enqueue(EMediaEvent::PlaybackResumed);
        if (this->paused) { //如果已经暂停，则执行以下操作
            if (this->paused) {
                this->frame_timer += av_gettime_relative() / 1000000.0 - this->vidclk.last_updated;
                if (this->read_pause_return != AVERROR(ENOSYS)) {
                    this->vidclk.paused = 0;
                }
                this->vidclk.Set(this->vidclk.Get(), this->vidclk.serial);
            }
            this->extclk.Set(this->extclk.Get(), this->extclk.GetSerial());
            UE_LOG(LogFFmpegMedia, Warning, TEXT("Player %p: SetRate =1 this->paused %d"), this, 0);
            this->paused = 0;
        }
        if (this->eof) {
            this->stream_seek(start_time != AV_NOPTS_VALUE ? start_time : 0, 0, 0);
        }
    }

    return true;
}


#undef LOCTEXT_NAMESPACE
