#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Restore Chinese/merged lines in moba.lua after a PowerShell encoding round-trip."""

PATH = r"E:\game\projects\moba\assets\scripts\moba.lua"
SEP = "-- " + "=" * 74

REP = {
    1: ("-- NeonMOBA —— League of Legends 风格 1v1 单路 MOBA（全部玩法在 Lua 中，数据驱动）。\n"
        "-- 地图: 真实召唤师峡谷（蓝方 -Z，红方 +Z，中路沿 Z 轴）。\n"
        "-- 玩家: 左键移动/锁敌，QWER 技能，自动普攻；滚轮缩放，空格回中；P 商店，B 回城，Tab 记分板。\n"
        "-- 敌方: AI 英雄 + 定时兵线 + 野区；推掉敌方水晶获胜。"),
    18: "-- 草丛（视野遮挡）。位置为世界坐标近似值。\nlocal BRUSHES = {",
    24: "-- 相机视角（俯角 pitch），位置由脚本每帧驱动 Main Camera 实体（保持鼠标可见）。\n"
        "local CAM = { yaw = math.pi, pitch = 0.82, dist = 36, minDist = 18, maxDist = 70 }",
    35: "-- 出场英雄（AI 对手从 AI_CHAMP 开始）",
    51: "-- 状态\n" + SEP,
    52: "local units = {}       -- 所有存活单位\nlocal heroes = {}      -- 英雄",
    53: "local structures = {}  -- 塔 / 水晶",
    75: "-- 音效（按事件触发 + 限流）\nlocal sfxLast = {}",
    101: "-- 用 Data Dragon 的真实英雄/技能数据（名称/图标/说明/冷却/消耗）覆盖演示数值。\n"
         "local function loadAbilityData()",
    144: "-- 屏幕像素 -> 地面 y=0 的世界点（相机由脚本固定，直接解析求交）。\n"
         "local function groundPick(sx, sy)",
    244: '            id = nid(), ent = ent, kind = "tower", team = team, name = "防御塔",',
    257: "-- 野区营地（中立单位，team=0）。\nlocal JUNGLE = {",
    258: '    { key = "red", name = "红BUFF", prefab = "unit_sru_red", x = -14, z = -22, hp = 2300, ad = 80, range = 2.0, radius = 0.8, h = 1.6, gold = 100, xp = 110, buff = "red" },',
    260: '    { key = "wolf", name = "魔沼蛙", prefab = "unit_sru_murkwolf", x = -11, z = -30, hp = 1300, ad = 45, range = 2.0, radius = 0.6, h = 1.2, gold = 60, xp = 70, buff = "" },',
    261: '    { key = "crab", name = "迅捷蟹", prefab = "unit_sru_crab", x = -9, z = 0, hp = 1200, ad = 40, range = 2.0, radius = 0.6, h = 1.0, gold = 55, xp = 60, buff = "" },',
    288: "-- 伤害 / 奖励 / 死亡",
    487: "-- 目标选择",
    547: "-- 状态（Lua 侧）",
    596: "-- 投射物 / AoE",
    627: "-- 技能\n" + SEP,
    631: '    if (h.cds[idx] or 0) > 0 then floatAt(h, "冷却中", false); return end',
    730: '        if tgt == nil then floatAt(h, "无目标", false); return end',
    738: "-- 更新",
    1201: "-- HUD (on_render, 1280x720 设计坐标)",
    1312: '        DrawText(string.format("复活中 %.1fs", math.max(0, h.dying)), cx, vh / 2, 30, 1, 0.3, 0.3, 1, true, true)',
    1320: '        DrawText("按 Enter / 空格 重新开始", cx, vh / 2 + 30, 20, 1, 1, 1, 1, true, true)',
    1349: '    DrawText("记分板  (Tab)", vw * 0.5, 88, 22, 0.95, 0.82, 0.35, 1, true, true)',
    1360: '            DrawText("装备", vw * 0.5 + 30, y + 8, 14, 0.9, 0.85, 0.6, 1, false, true)',
    1385: '    DrawText("装备商店", vw * 0.5, oy - 48, 24, 0.95, 0.82, 0.35, 1, true, true)',
    1386: '    DrawText("点击购买，P 关闭", vw * 0.5, oy - 24, 14, 0.85, 0.85, 0.85, 1, true, true)',
    1410: '    DrawText("方向键 / 数字键 / 鼠标点击选择，Enter 或 Q 确认", vw * 0.5, 92, 15, 0.85, 0.85, 0.85, 1, true, true)',
    1453: '    DrawText("加载中…", vw * 0.5, vh - 44, 15, 0.9, 0.85, 0.6, 1, true, true)',
}


def main():
    with open(PATH, encoding="utf-8") as fh:
        lines = fh.read().split("\n")
    for ln, new in REP.items():
        lines[ln - 1] = new
    text = "\n".join(lines)
    with open(PATH, "w", encoding="utf-8", newline="") as fh:
        fh.write(text)
    print("fixed %d lines; U+FFFD left: %d" % (len(REP), text.count("\ufffd")))


if __name__ == "__main__":
    main()
