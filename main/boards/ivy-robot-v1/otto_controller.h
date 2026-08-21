#ifndef OTTO_CONTROLLER_H
#define OTTO_CONTROLLER_H

struct HardwareConfig;

// 花花舞/打节拍/随机舞蹈 音乐联动入口（由 miot_client / music_player 调用）
void OttoControllerQueueFlowerDance();
void OttoControllerQueueBeatKeeping();
void OttoControllerQueueRandomDance();
// 连续舞蹈：跟随音乐持续跳 SafeGroove，音乐停止时由 OttoControllerStopAll 结束
void OttoControllerQueueContinuousDance();
// 立即停止当前动作并清空动作队列（由按键唤醒等打断入口调用）
void OttoControllerStopAll();
bool OttoControllerAvailable();

#endif // OTTO_CONTROLLER_H
