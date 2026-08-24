// NeonPvZ 游戏内 HUD (JS 脚本后端示例)
// 用 on_render 2D 画布绘制: 阳光 / 植物按钮 / 波次 / 提示与胜负横幅。
// 本地化文案经 Loc(key) 读取 (locales/*.json)。

function on_render() {
  var sun = GetVar("sun");
  if (typeof sun !== "number") sun = 0;
  var selected = GetVar("selected");
  var wave = GetVar("wave");
  if (typeof wave !== "number") wave = 0;
  var gameover = GetVar("gameover") === true;
  var started = GetVar("started") === true;

  // 顶部 HUD 条
  DrawRect(0, 0, 1280, 58, 0.10, 0.12, 0.16, 0.92);
  DrawRect(0, 56, 1280, 2, 0.35, 0.65, 1, 1);

  // 阳光计数
  DrawSprite("assets/sprites/sun.png", 14, 8, 40, 40, false, false);
  DrawText(String(sun), 66, 16, 26, 1, 0.95, 0.25, 1);

  // 植物按钮 (1-4)
  var plants = [
    { type: "sunflower",  label: "1 向日葵 50" },
    { type: "peashooter", label: "2 豌豆 100" },
    { type: "snowpea",    label: "3 寒冰 175" },
    { type: "wallnut",    label: "4 坚果 50" }
  ];
  var bx = 210;
  for (var i = 0; i < plants.length; i++) {
    var active = selected === plants[i].type;
    DrawRect(bx, 6, 152, 44, active ? 0.22 : 0.16, active ? 0.40 : 0.22,
             active ? 0.62 : 0.30, 0.95);
    DrawText(plants[i].label, bx + 8, 17, 16, 0.9, 0.92, 1, 1);
    bx += 158;
  }

  DrawText(Loc("wave").replace("%d", wave), 1000, 16, 22, 0.8, 0.85, 1, 1);

  if (gameover) {
    DrawRect(340, 300, 600, 120, 0.35, 0.1, 0.1, 0.9);
    DrawText(Loc("gameover"), 640, 340, 40, 1, 0.5, 0.5, 1, true, true);
  }
  if (!started && !gameover) {
    DrawText(Loc("select"), 640, 660, 18, 0.85, 0.9, 1, 0.85, true, true);
  }
}
