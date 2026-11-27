#include "bambu_cmd.h"

#include <stdio.h>
#include <string.h>

static int fit(int n, size_t cap)
{
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

static int simple_print_cmd(char *buf, size_t cap, uint32_t seq, const char *cmd)
{
    return fit(snprintf(buf, cap, "{\"print\":{\"sequence_id\":\"%lu\",\"command\":\"%s\",\"param\":\"\"}}",
                        (unsigned long)seq, cmd), cap);
}

int bbl_cmd_pushall(char *buf, size_t cap, uint32_t seq)
{
    return fit(snprintf(buf, cap,
                        "{\"pushing\":{\"sequence_id\":\"%lu\",\"command\":\"pushall\",\"version\":1,\"push_target\":1}}",
                        (unsigned long)seq), cap);
}

int bbl_cmd_pause(char *buf, size_t cap, uint32_t seq) { return simple_print_cmd(buf, cap, seq, "pause"); }
int bbl_cmd_resume(char *buf, size_t cap, uint32_t seq) { return simple_print_cmd(buf, cap, seq, "resume"); }
int bbl_cmd_stop(char *buf, size_t cap, uint32_t seq) { return simple_print_cmd(buf, cap, seq, "stop"); }

int bbl_cmd_speed(char *buf, size_t cap, uint32_t seq, int level)
{
    if (level < 1) level = 1;
    if (level > 4) level = 4;
    return fit(snprintf(buf, cap, "{\"print\":{\"sequence_id\":\"%lu\",\"command\":\"print_speed\",\"param\":\"%d\"}}",
                        (unsigned long)seq, level), cap);
}

int bbl_cmd_light(char *buf, size_t cap, uint32_t seq, bool on)
{
    return fit(snprintf(buf, cap,
                        "{\"system\":{\"sequence_id\":\"%lu\",\"command\":\"ledctrl\",\"led_node\":\"chamber_light\","
                        "\"led_mode\":\"%s\",\"led_on_time\":500,\"led_off_time\":500,\"loop_times\":0,\"interval_time\":0}}",
                        (unsigned long)seq, on ? "on" : "off"), cap);
}

int bbl_cmd_gcode(char *buf, size_t cap, uint32_t seq, const char *gcode)
{
    if (strpbrk(gcode, "\"\\")) return -1;
    return fit(snprintf(buf, cap, "{\"print\":{\"sequence_id\":\"%lu\",\"command\":\"gcode_line\",\"param\":\"%s\\n\"}}",
                        (unsigned long)seq, gcode), cap);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

int bbl_cmd_nozzle_temp(char *buf, size_t cap, uint32_t seq, int celsius)
{
    char g[24];
    snprintf(g, sizeof(g), "M104 S%d", clampi(celsius, 0, 300));
    return bbl_cmd_gcode(buf, cap, seq, g);
}

int bbl_cmd_bed_temp(char *buf, size_t cap, uint32_t seq, int celsius)
{
    char g[24];
    snprintf(g, sizeof(g), "M140 S%d", clampi(celsius, 0, 110));
    return bbl_cmd_gcode(buf, cap, seq, g);
}

int bbl_cmd_part_fan(char *buf, size_t cap, uint32_t seq, int percent)
{
    char g[24];
    percent = clampi(percent, 0, 100);
    snprintf(g, sizeof(g), "M106 P1 S%d", (255 * percent + 50) / 100);
    return bbl_cmd_gcode(buf, cap, seq, g);
}
