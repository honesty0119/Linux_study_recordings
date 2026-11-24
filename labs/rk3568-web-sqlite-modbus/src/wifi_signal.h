/* Wi-Fi信号读取：固定调用系统iw，只读、限时、无shell。 */
#ifndef TASK5_WIFI_SIGNAL_H
#define TASK5_WIFI_SIGNAL_H
int wifi_parse_signal(const char *text, int *dbm);
int wifi_read_signal(const char *interface_name, int *dbm);
#endif
