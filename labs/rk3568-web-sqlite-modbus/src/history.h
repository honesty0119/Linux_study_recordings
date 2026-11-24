/* SQLite历史单写者：采集路径仅向有界队列复制快照，不等待数据库IO。 */
#ifndef TASK5_HISTORY_H
#define TASK5_HISTORY_H
#include <stdint.h>
int history_start(const char *path, unsigned interval_seconds);
void history_offer(const uint16_t sample[64]);
unsigned history_diagnostics(void);
int history_finish(void);
#endif
