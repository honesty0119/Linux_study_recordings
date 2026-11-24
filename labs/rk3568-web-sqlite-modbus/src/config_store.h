/* 配置库与运行事实：配置API是独立config.db的唯一写者；运行进程只读加载。
 * revision用于并发更新保护；运行状态文件位于私有/tmp目录，不作为持久事实。 */
#ifndef TASK5_CONFIG_STORE_H
#define TASK5_CONFIG_STORE_H
#include <stdint.h>
struct task_config { int revision,unit,baud,data_bits,stop_bits,history_seconds; char parity; };
struct task_runtime { int revision,unit,history_seconds,history_enabled; uint64_t mono_ms,session; };
int config_valid(const struct task_config *c);
int config_init(const char *path);
int config_load(const char *path,struct task_config *c);
/* HTTP式结果：200提交成功，409旧版本，400参数不合法，503数据库不可用。 */
int config_save(const char *path,int expected,struct task_config *c);
int runtime_claim(const char *path);
int runtime_write(const char *path,const struct task_runtime *r);
int runtime_read(const char *path,struct task_runtime *r);
#endif
