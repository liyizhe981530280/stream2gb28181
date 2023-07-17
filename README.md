# stream2gb28181
将输入的网络流(rtsp，rtmp，http流等)转成国标流(rtp+ps)发送出去

实现将输入的网络流，转成国标流发送出去 由C代码实现，仅在linux做了测试，由于没有用到linux系统调用，windows应该也是可以运行的。 拉流利用ffmpeg，转ps流利用libmpeg。rtp发送是自己写的

如何测试？

1，debug模式下(见makefile)，会将流转成ps流之后，打包成rtp包发送出去的同时，也会保存到out.mpg中。可以利用播放器播放这个文件。

2，同时也可以利用zlmeditkit这个流媒体服务(可在github中下载)，将此程序的发送端口改成10000

例如，发送的ssrc是1234，那么zlmediakit会将这个流转成rtsp,rtmp，http流。以rtsp为例：url为rtsp://127.0.0.1/rtp/000004D2(ssrc的16进制)， 可以利用ffplay或者vlc播放这个url

注意：国标流并不是国际标准的流，所以一些播放器对国标流支持的不好，当国标流中有声音的时候，有可能播放有异常。
