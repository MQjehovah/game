// Wave director (runtime JS plugin). Advances a wave every INTERVAL seconds up
// to MAX_WAVES, communicating with the game script through the plugin-scoped
// GameVar ("plugin:wave_director:wave" / "plugin:wave_director:won"). Survive
// every wave to win.
var elapsed = 0;
var wave = 0;
var INTERVAL = 10; // seconds between waves
var MAX_WAVES = 8; // survive this many waves to win

function on_load() {
  Plugin.On("tick", function (dt) {
    if (wave >= MAX_WAVES) return; // final wave already launched
    elapsed += dt;
    if (elapsed >= INTERVAL) {
      elapsed = 0;
      wave = wave + 1;
      Plugin.SetVar("wave", wave);
      Plugin.Log("info", "第 " + wave + " 波即将来袭");
      if (wave >= MAX_WAVES) {
        Plugin.SetVar("won", true);
        Plugin.Log("info", "全部波次结束，胜利！");
      }
    }
  });

  // Module API for other plugins / the host.
  Plugin.Export("currentWave", function () { return wave; });
  Plugin.Export("maxWaves", function () { return MAX_WAVES; });
}
