// NeonPvZ HUD (JS back-end). Draws sun / plant buttons (with seed cooldowns +
// selected + cost) / shovel / wave / pause / win-loss screens. Reads game vars
// written by the Lua master script, so the two back-ends stay decoupled.
function on_render() {
  var sun = GetVar("sun");
  if (typeof sun !== "number") sun = 0;
  var selected = GetVar("selected");
  var wave = GetVar("wave");
  if (typeof wave !== "number") wave = 0;
  var gameover = GetVar("gameover") === true;
  var won = GetVar("plugin:wave_director:won") === true;
  var started = GetVar("started") === true;
  var paused = GetVar("paused") === true;

  // Top HUD bar.
  DrawRect(0, 0, 1280, 58, 0.10, 0.12, 0.16, 0.92);
  DrawRect(0, 56, 1280, 2, 0.35, 0.65, 1, 1);

  // Sun counter.
  DrawSprite("assets/sprites/sun.png", 14, 8, 40, 40, false, false);
  DrawText(String(sun), 66, 16, 26, 1, 0.95, 0.25, 1);

  // Plant seed packets (key order matches input.json: 1..6) + shovel.
  var plants = [
    { type: "sunflower",  label: "1 向日葵 50",  maxCd: 4 },
    { type: "peashooter", label: "2 豌豆 100",   maxCd: 4 },
    { type: "snowpea",    label: "3 寒冰 175",   maxCd: 4 },
    { type: "wallnut",    label: "4 坚果 50",    maxCd: 10 },
    { type: "repeater",   label: "5 双发 200",   maxCd: 6 },
    { type: "cherrybomb", label: "6 樱桃 150",   maxCd: 30 }
  ];
  var bx = 240;
  for (var i = 0; i < plants.length; i++) {
    var active = selected === plants[i].type;
    DrawRect(bx, 6, 130, 44, active ? 0.22 : 0.16, active ? 0.40 : 0.22,
             active ? 0.62 : 0.30, 0.95);
    DrawText(plants[i].label, bx + 6, 13, 15, 0.9, 0.92, 1, 1);
    var cd = GetVar("cooldown_" + plants[i].type);
    if (typeof cd !== "number") cd = 0;
    if (cd > 0.01) {
      var frac = Math.min(1, cd / plants[i].maxCd);
      DrawRect(bx, 6, 130, 44 * frac, 0.05, 0.06, 0.10, 0.75);
      DrawText(Math.ceil(cd) + "s", bx + 6, 6, 13, 1, 0.7, 0.4, 1);
    }
    bx += 136;
  }
  var shovelActive = selected === "shovel";
  DrawRect(bx + 2, 6, 60, 44, shovelActive ? 0.3 : 0.18, 0.3, 0.3, 0.95);
  DrawText("铲子", bx + 8, 16, 13, 1, 0.9, 0.9, 1);

  DrawText(Loc("wave").replace("%d", wave), 1010, 16, 20, 0.8, 0.85, 1, 1);

  // End-of-game screens.
  if (gameover && won) {
    DrawRect(340, 280, 620, 170, 0.10, 0.35, 0.15, 0.92);
    DrawText(Loc("win"), 640, 320, 44, 0.8, 1, 0.7, 1, true, true);
    DrawText(Loc("restart_hint"), 640, 395, 16, 0.85, 0.95, 1, 0.95, true, false);
  } else if (gameover) {
    DrawRect(340, 280, 620, 150, 0.35, 0.1, 0.1, 0.9);
    DrawText(Loc("gameover"), 640, 315, 40, 1, 0.5, 0.5, 1, true, true);
    DrawText(Loc("restart_hint"), 640, 385, 16, 0.9, 0.9, 1, 0.95, true, false);
  }
  if (paused && !gameover) {
    DrawRect(0, 0, 1280, 720, 0, 0, 0, 0.4);
    DrawText("暂停中", 640, 360, 44, 1, 1, 1, 1, true, true);
  }
  if (!started && !gameover) {
    DrawText(Loc("select"), 640, 660, 18, 0.85, 0.9, 1, 0.85, true, true);
  }
}
