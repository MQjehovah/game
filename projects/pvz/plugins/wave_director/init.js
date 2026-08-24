// 波次导演 (runtime JS 插件示例)
// 一个"玩法系统"插件: 每 12 秒推进一波, 通过插件作用域 GameVar
// (plugin:wave_director:wave) 与游戏脚本通信; 游戏脚本轮询该值并出怪。
// 插件状态与场景脚本完全隔离 (Plugin.SetVar 自动加 plugin:<id>: 前缀)。

var elapsed = 0;
var wave = 0;
var INTERVAL = 12; // seconds between waves

function on_load() {
  Plugin.On("tick", function (dt) {
    elapsed += dt;
    if (elapsed >= INTERVAL) {
      elapsed = 0;
      wave = wave + 1;
      Plugin.SetVar("wave", wave);
      Plugin.Log("info", "第 " + wave + " 波即将来袭");
    }
  });

  // 模块 API: 其他插件/宿主可经 Plugin.Call("wave_director", "currentWave")
  // 查询当前波次。
  Plugin.Export("currentWave", function () {
    return wave;
  });
}
