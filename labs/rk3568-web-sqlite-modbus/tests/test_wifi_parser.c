/* Wi-Fi文本边界测试：不调用实际iw，不改变网络。 */
#include <assert.h>
#include <stdio.h>
#include "wifi_signal.h"
int main(void)
{
    int dbm=99;
    assert(wifi_parse_signal("Connected\n\tsignal: -24 dBm\n\ttx bitrate: 300.0 MBit/s\n",&dbm)==0 && dbm==-24);
    assert(wifi_parse_signal("signal: -127 dBm",&dbm)==0 && dbm==-127);
    assert(wifi_parse_signal("signal: 0 dBm",&dbm)==0 && dbm==0);
    const char *bad[]={"Not connected.\n","signal: -24", "signal: 90 %","signal: -128 dBm","signal: 1 dBm","signal: -24.5 dBm","signal: 999999999999999999999999999999 dBm","signal: -24 dBm junk"};
    for (unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) assert(wifi_parse_signal(bad[i],&dbm)<0);
    puts("WIFI_PARSER_PASS 11");return 0;
}
