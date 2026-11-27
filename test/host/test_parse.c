// 电脑上运行的解析/命令单元测试：make -C test/host
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bambu_cmd.h"
#include "bambu_parse.h"

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(*len + 1);
    assert(fread(buf, 1, *len, f) == *len);
    buf[*len] = 0;
    fclose(f);
    return buf;
}

#define PARSE(s, str) bbl_parse_report((s), (str), strlen(str))

int main(void)
{
    bbl_status_t s;
    bbl_status_init(&s);

    // 1. 全量帧（ha-bambulab 的 A1 mock 数据）
    size_t len;
    char *full = read_file("a1_pushall.json", &len);
    assert(bbl_parse_report(&s, full, len));
    assert(s.valid);
    assert(s.gcode_state == BBL_GS_IDLE);
    assert(s.speed_level == 2);
    assert(s.nozzle > 27.8f && s.nozzle < 27.9f);
    assert(s.bed > 29.7f && s.bed < 29.8f);
    assert(s.has_light && !s.light_on);
    assert(s.wifi_dbm == -48);
    assert(s.flag3_auth);              // flag3 = 31，带 bit4
    assert(s.signature_required);      // 没有 fun 字段时按 flag3 判断
    assert(s.ams_trays == 4);
    assert(s.tray[0].loaded && !strcmp(s.tray[0].type, "PLA"));
    assert(s.tray[0].color == 0x76D9F4);
    assert(s.tray[3].color == 0x0ACC38);
    assert(s.tray[0].remain == 100);
    assert(s.tray_now == 255);
    free(full);

    // 2. 增量帧：只更新出现的字段
    assert(PARSE(&s, "{\"print\":{\"gcode_state\":\"RUNNING\",\"mc_percent\":42,\"mc_remaining_time\":75,"
                     "\"layer_num\":12,\"total_layer_num\":150,\"cooling_fan_speed\":\"15\","
                     "\"subtask_name\":\"测试模型\",\"command\":\"push_status\"}}"));
    assert(s.gcode_state == BBL_GS_RUNNING);
    assert(s.percent == 42 && s.remaining_min == 75);
    assert(s.layer == 12 && s.total_layers == 150);
    assert(s.part_fan_pct == 100);
    assert(!strcmp(s.subtask, "测试模型"));
    assert(s.bed > 29.7f);             // 没出现的字段保持不变
    assert(s.signature_required);      // flag3 跨帧保留

    assert(PARSE(&s, "{\"print\":{\"gcode_start_time\":\"1681479206\",\"ams\":{\"tray_now\":\"2\"},\"command\":\"push_status\"}}"));
    assert(s.start_time == 1681479206);
    assert(s.tray_now == 2 && s.ams_trays == 4);   // 只有 tray_now 的增量不清空料位

    // 料位增量：只带 remain 的条目不能把材料清掉；只有 id 的条目表示空料位
    PARSE(&s, "{\"print\":{\"ams\":{\"ams\":[{\"id\":\"0\",\"tray\":[{\"id\":\"1\",\"remain\":50},{\"id\":\"2\"}]}]}}}");
    assert(s.ams_trays == 4);
    assert(s.tray[1].loaded && !strcmp(s.tray[1].type, "PLA") && s.tray[1].remain == 50 && s.tray[1].color == 0xFFF144);
    assert(!s.tray[2].loaded);
    assert(s.tray[3].loaded);

    // 塔可的A1 实测：有料盘但没设置类型（tray_exist_bits=5）→ present 但不 loaded；外挂料盘 vt_tray
    {
        bbl_status_t t;
        bbl_status_init(&t);
        assert(PARSE(&t, "{\"print\":{\"ams\":{\"ams\":[{\"id\":\"0\",\"tray\":[{\"id\":\"0\",\"remain\":0,\"tray_type\":\"\",\"tray_color\":\"00000000\"},{\"id\":\"1\"},{\"id\":\"2\",\"remain\":0,\"tray_type\":\"\",\"tray_color\":\"00000000\"},{\"id\":\"3\"}]}],\"tray_exist_bits\":\"5\",\"tray_now\":\"255\"},\"vt_tray\":{\"id\":\"254\",\"tray_type\":\"PETG\",\"tray_color\":\"5A657BFF\"},\"command\":\"push_status\"}}"));
        assert(t.ams_trays == 4);
        assert(t.tray[0].present && !t.tray[0].loaded);
        assert(!t.tray[1].present);
        assert(t.tray[2].present && !t.tray[2].loaded);
        assert(!t.tray[3].present);
        assert(t.vt.loaded && !strcmp(t.vt.type, "PETG") && t.vt.color == 0x5A657B && t.vt.remain == -1);
        assert(!PARSE(&t, "{\"print\":{\"vt_tray\":{\"id\":\"254\",\"tray_type\":\"PETG\"},\"command\":\"push_status\"}}"));
    }

    // 3. 重复帧：没有变化
    assert(!PARSE(&s, "{\"print\":{\"mc_percent\":42,\"command\":\"push_status\"}}"));

    // 4. fun 字段优先于 flag3：开发者模式已开
    assert(PARSE(&s, "{\"print\":{\"fun\":\"3EC18FFF9CFF\",\"command\":\"push_status\"}}"));
    assert(!s.signature_required);
    assert(PARSE(&s, "{\"print\":{\"fun\":\"3EC1AFFF9CFF\",\"command\":\"push_status\"}}"));
    assert(s.signature_required);

    // 5. 命令回执
    uint32_t seq0 = s.result_seq;
    assert(PARSE(&s, "{\"print\":{\"command\":\"pause\",\"sequence_id\":\"5\",\"result\":\"failed\","
                     "\"reason\":\"mqtt message verify failed\",\"err_code\":84033543}}"));
    assert(s.result_seq == seq0 + 1);
    assert(!s.result_ok && !strcmp(s.result_cmd, "pause"));
    assert(s.result_err_code == 84033543u);
    assert(s.gcode_state == BBL_GS_RUNNING);   // 回执不应改状态
    assert(PARSE(&s, "{\"system\":{\"command\":\"ledctrl\",\"led_node\":\"chamber_light\",\"result\":\"success\"}}"));
    assert(s.result_ok && !strcmp(s.result_cmd, "ledctrl"));

    // 6. 灯光状态
    assert(PARSE(&s, "{\"print\":{\"lights_report\":[{\"node\":\"chamber_light\",\"mode\":\"on\"}],\"command\":\"push_status\"}}"));
    assert(s.light_on);

    // 7. 坏 JSON 不崩
    assert(!PARSE(&s, "{\"print\":"));

    // 8. 命令拼装
    char buf[256];
    assert(bbl_cmd_pause(buf, sizeof(buf), 7) > 0);
    assert(!strcmp(buf, "{\"print\":{\"sequence_id\":\"7\",\"command\":\"pause\",\"param\":\"\"}}"));
    assert(bbl_cmd_nozzle_temp(buf, sizeof(buf), 1, 220) > 0);
    assert(strstr(buf, "\"param\":\"M104 S220\\n\""));
    assert(bbl_cmd_bed_temp(buf, sizeof(buf), 1, 500) > 0 && strstr(buf, "M140 S110"));
    assert(bbl_cmd_part_fan(buf, sizeof(buf), 1, 50) > 0 && strstr(buf, "M106 P1 S128"));
    assert(bbl_cmd_speed(buf, sizeof(buf), 1, 3) > 0 && strstr(buf, "\"param\":\"3\""));
    assert(bbl_cmd_light(buf, sizeof(buf), 1, true) > 0 && strstr(buf, "\"led_mode\":\"on\""));
    assert(bbl_cmd_gcode(buf, sizeof(buf), 1, "M117 \"x\"") == -1);
    assert(bbl_cmd_pause(buf, 10, 7) == -1);

    printf("all host tests passed\n");
    return 0;
}
