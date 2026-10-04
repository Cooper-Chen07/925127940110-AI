#include "UsrAI.h"
#include <QString>
#include<set>
#include <iostream>
#include<unordered_map>
#include<list>
#include <cstdlib>
// 3.0.7g 模板补齐的标准头（防编译缺失）
#include <cmath>
#include <climits>
#include <algorithm>
#include <vector>
#include <map>
#include <fstream>   // 【诊断】AI 自己写 ai_log.txt（stdout 在本地 GUI 下抓不到 ✗）

using namespace std;

// ============================================================
// 【提交排查·实验C】AI 诊断输出开关
//   DebugText → 引擎的 call_debugText/Logger 是**在 AI 线程里**调用的；评测机崩溃日志显示：
//   主线程当时正在 Logger::messageOutput 写日志，AI 线程同时崩在 UsrAI::processData
//   → 怀疑日志系统非线程安全（本地 MinGW 32 位/时序不同，才没暴露）。
//   1 = 打开诊断行（本地看状态用）；0 = 关闭（提交用）
// ============================================================
#define USRAI_DEBUG_LINE 0
tagGame tagUsrGame;
ins UsrIns;
/*##########DO NOT MODIFY THE CODE ABOVE##########*/

// ============================================================
// 【3.0.7g·改造A】所有跨帧状态放在**文件作用域**（类里不再有任何数据成员）
//   原因见 UsrAI.h 顶部注释：评测机若把本文件链接到预编译好的引擎，
//   类里加成员会让 new UsrAI() 的 sizeof 与引擎预期不符 → 对象越界写坏堆 → 开局崩溃。
//   这里用与原来**同名**的变量，所以下面所有函数体一行都不用改。
// ============================================================
static int m_scoutIdx = 0;                              // 已完成探路次数（跨帧保存）
static int m_scoutStartFrame = -1;                      // 探路下令帧（卡住超时判断用）
static int m_centerX = -1, m_centerY = -1;              // 市镇中心块坐标（回家/找地参照）
static int m_map[100][100] = {};                        // 地图标记：0=空地 >0=占用 <0=不可走
static std::vector<Point> m_explored;                   // 已探明的空地集合
static int m_searchX = 0, m_searchY = 0;                // 找建筑空地的搜索起点
static const int MAX_HUNTER_PER_PREY = 2;               // 每只活物最多猎人
static std::set<int> m_foodGatherers;                   // 专属食物采集者
static std::unordered_map<int,int> m_role;              // 农民SN -> 工种
static int m_depotBuilderSN = -1;                       // 资源点仓库/谷仓专职建造者
static bool m_preyStockDone = false;                    // 猎物仓库只建一次
static int m_preyStockFrame = -1;                       // 上次下令建猎物仓库的帧
static int m_lastDebugFrame = -9999;                    // 上次打诊断信息的帧
static int m_bronzeUpgradeFrame = -1;                   // 铜器升级下令帧
static std::set<int> m_issued;                          // 本帧已下令的对象 SN
// 【用户要求·后期人口分配】
//   后期人口估算：农民 20 + 祭司 1 + 军队 ≈29（人口上限 50）
//   → 后期食物总需求 ≈ 29 兵 × 40~70 食 + 科技 ≈ 2500 食 → 6 块田（一块一人）足够
//   农田太多 → 木头被农田吃掉 → 兵和建筑都造不动 ✗（用户原话）
static const int FARM_MAX_COUNT  = 8;    // 农田数量上限（用户要求：8 块）
static const int FARM_MIN_COUNT  = 3;    // 农田数量下限
static const int FARM_WOOD_GATE  = 200;  // 开新田的木头门槛：木头 < 这个数 → 先去伐木
                                         //   （实现用户要求："没食物可采/不能种田 → 先伐木，木头够了再种田"）
static const int GOLD_FARMERS_FIX = 3;   // 采金固定人数（黄金 ≥300 时降到 2）

// 【用户要求·大象组队】最少 3 人、最多 5 人同一帧一起上（安全优先 ✓）
static const int ELE_TEAM_MIN = 3;       // 不足 3 人 → 原地等队友（不等就是送死 ✗）
static const int ELE_TEAM_MAX = 5;       // 上限 5 人：人多 → 大象死得快 → 村民少挨打 ✓
// 【用户建议·大象留到后期，一次性调用多余伐木工】
//   ELE_CALL_FRAME：什么时候才开始动用"伐木富余人口"打大象（默认第二波 ≈9 分钟）
//   ELE_CALL_TEAM ：一次调用几个人（同一帧一起派 ✓ 三人同时开打才安全 ✓）
static const int ELE_CALL_FRAME = 13500;   // = FRAME_WAVE2（第二波 ≈9 分钟）
                                         //   注意：这里必须用字面量 ✗ —— FRAME_WAVE2 的
                                         //   #define 在文件靠后（约 :113），写宏名会编译报错 ✗
static const int ELE_CALL_TEAM  = 3;
static int m_eleCallSN = -1;             // 已经为哪头大象调用过伐木工（避免重复调用 ✓）
// 本帧已经派往这头大象的农民数（每帧清空 ✓）
//   为什么要它：帧首快照不随本帧派令更新 → 同一帧里所有空闲农民都会看到 c==0
//   → 会全部被派去同一头大象 ✗（与"多人挤一块农田"同源 ✓）
static const int HUNT_WAIT_MAX = 600;    // 【用户要求·别站着不动】等队友打猎的最长帧数（24 秒）
static std::map<int,int> m_huntWaitSince; // 每个农民"从哪一帧开始等队友"（超时就放弃等待 ✓）
static int m_eleSentSN = -1;             // 本帧正在组队的大象 SN
static int m_eleSentCount = 0;           // 本帧已派往它的人数

// 【跨模块·探路兵豁免】这两个量 defense / attackPhase 都要用 → 必须声明在它们之前 ✗
static int m_scoutUnitSN = -1;                 // 当前侦察兵 SN（-1=没有）
static int m_scoutDone = 0;                    // 1=探路任务结束（成功或放弃）

// 【跨模块·反攻】defense 也要看这两个量（反攻启动后它必须让位 ✓）
//   原来声明在 :3370（defense 在 :2218）→ defense 看不到 ✗ → 互相抢令 ✗
static int m_atkOn = 0;                       // 1=反攻已启动
static int m_atkPhase = 0;                    // 0=集结 1=推进拉扯 2=交战 3=冲锋
// 【修复·防御集结重复下令】记录每个兵上次被叫回集结点的帧（90 帧节流 ✓）
static std::map<int,int> m_homeRecallFrame;

// ===== 【用户要求·农民远处阵亡 → 标记危险区】=====
static const int DANGER_MAX       = 8;      // 最多记几个危险点
static const int DANGER_R         = 12;     // 危险点半径（格）—— 这一带都不再派人采
static const int DANGER_LIFE      = 3000;   // 危险点有效期（帧）= 2 分钟，过期自动失效 ✓
static const int FARMER_DEATH_FAR = 18;     // 阵亡点离家超过这么多格才算"远处采集被杀"
// 【用户要求·按时间判定波次】第二三波进攻窗口内、离家不远的伤亡不算危险点 ✓
//   注意：FRAME_WAVE2/3 两个宏定义在文件靠后 ✗ → 这里只能用具名常量+字面量 ✓
static const int DANGER_WAVE2_START = 13500;  // = FRAME_WAVE2（第二波）
static const int DANGER_WAVE3_START = 21000;  // = FRAME_WAVE3（第三波）
static const int WAVE_WIN_BEFORE    = 500;    // 波次前 20 秒就算进入窗口
static const int WAVE2_WIN_AFTER    = 4000;   // 第二波后 160 秒
static const int WAVE3_WIN_AFTER    = 6000;   // 第三波后 240 秒
static const int WAVE_FAR           = 35;     // 波次期间"离得很远"的判定（格）：超过它才算危险 ✓
static int m_dangerX[DANGER_MAX];
static int m_dangerY[DANGER_MAX];
static int m_dangerF[DANGER_MAX];           // 记录时间（帧），用于过期判定
static int m_dangerN = 0;
static std::map<int,int> m_farmerPX, m_farmerPY;   // 上一帧各农民位置（块坐标）

// 逐帧调用：记录农民位置、发现远处阵亡就记危险点 ✓
static void farmerDeathWatch(const tagInfo& info)
{
    const int hx = (m_centerX >= 0) ? m_centerX : MAP_L / 2;
    const int hy = (m_centerY >= 0) ? m_centerY : MAP_U / 2;
    std::map<int,int> nowX, nowY;
    for (const tagFarmer& f : info.farmers) {
        nowX[f.SN] = (int)(f.DR / BLOCKSIDELENGTH);
        nowY[f.SN] = (int)(f.UR / BLOCKSIDELENGTH);
    }
    // 上一帧有、这一帧没了 → 阵亡（或被我方/敌方转化，效果一样：那地方有敌人 ✓）
    for (std::map<int,int>::iterator it = m_farmerPX.begin(); it != m_farmerPX.end(); ++it) {
        if (nowX.count(it->first)) continue;                 // 还活着
        const int ddx = it->second - hx;
        const int ddy = m_farmerPY[it->first] - hy;
        if (ddx * ddx + ddy * ddy <= FARMER_DEATH_FAR * FARMER_DEATH_FAR) continue;   // 近处死亡 → 不算
        // 【用户要求·两种不算】① 被大象/狮子等猛兽打死 ② 被第二三波正规军打死
        //   这两种死法都不代表"这处资源被敌人占了" ✗ → 不标记危险区 ✓
        bool excluded = false;
        for (const tagResource& mBeast : info.resources) {          // ① 6 格内有活着的猛兽
            if (mBeast.Type != RESOURCE_ELEPHANT && mBeast.Type != RESOURCE_LION) continue;
            if (mBeast.Blood <= 0) continue;                        // 死掉的动物不算 ✓
            const int bax = mBeast.BlockDR - it->second;
            const int bay = mBeast.BlockUR - m_farmerPY[it->first];
            if (bax * bax + bay * bay <= 36) { excluded = true; break; }
        }
        if (!excluded) {
            // ② 【用户要求·按时间判定】第二/三波进攻窗口内 且 离家不远 → 不算危险点 ✗
            //    （波次打到家门口，伤亡是正常的；否则附近资源会被整片误封 ✗）
            //    但"死得很远"（> WAVE_FAR 格）→ 仍算 ✓ —— 说明那处资源确实危险 ✓
            const int fNow = info.GameFrame;
            const bool inWave2 = (fNow >= DANGER_WAVE2_START - WAVE_WIN_BEFORE
                                  && fNow <= DANGER_WAVE2_START + WAVE2_WIN_AFTER);
            const bool inWave3 = (fNow >= DANGER_WAVE3_START - WAVE_WIN_BEFORE
                                  && fNow <= DANGER_WAVE3_START + WAVE3_WIN_AFTER);
            if ((inWave2 || inWave3)
                && (ddx * ddx + ddy * ddy <= WAVE_FAR * WAVE_FAR)) excluded = true;
        }
        if (excluded) continue;                                     // 不算 → 不记危险点 ✓
        int slot = -1;
        for (int i = 0; i < m_dangerN; ++i) {                 // 5 格内已有旧点 → 只刷新计时
            const int ex = m_dangerX[i] - it->second;
            const int ey = m_dangerY[i] - m_farmerPY[it->first];
            if (ex * ex + ey * ey <= 25) { slot = i; break; }
        }
        if (slot < 0) {
            if (m_dangerN < DANGER_MAX) slot = m_dangerN++;
            else {                                            // 满了 → 覆盖最旧的
                slot = 0;
                for (int i = 1; i < DANGER_MAX; ++i) if (m_dangerF[i] < m_dangerF[slot]) slot = i;
            }
        }
        m_dangerX[slot] = it->second;
        m_dangerY[slot] = m_farmerPY[it->first];
        m_dangerF[slot] = info.GameFrame;
    }
    m_farmerPX.swap(nowX);                                   // 更新成"本帧"位置，供下一帧比对 ✓
    m_farmerPY.swap(nowY);
}


static std::set<int> m_farmTaken;                       // 【修复·挤同一块田】本帧已经被派了人的农田 SN
                                                        //   快照 info 是帧首的，派出去的人不会立刻反映进来，
                                                        //   所以必须自己记账，否则同一帧会把多个人派到同一块田 ✗
static int m_builderSN = -1;                            // 专职建造村民 SN
static std::unordered_map<int,int> m_moveStart;         // 农民SN -> 开始移动帧
static std::unordered_map<int,double> m_lastDist;       // 农民SN -> 上帧到目标距离
static std::unordered_map<int,int> m_stuckFrame;        // 农民SN -> 上次判卡住帧
static std::unordered_map<int,int> m_orderFrame;        // 农民SN -> 上次下采集令帧
static std::unordered_map<int,int> m_orderTarget;       // 农民SN -> 上次下令目标SN
static std::unordered_map<int,int> m_recoverFrame;      // 农民SN -> 强制回家帧
static std::unordered_map<int,double> m_orderX;         // 农民SN -> 下令时位置X
static std::unordered_map<int,double> m_orderY;         // 农民SN -> 下令时位置Y
static bool m_huntWaiting = false;                      // 想打猎但缺搭档
static std::unordered_map<int,int> m_badTarget;         // 资源SN -> 判定不可达帧
static std::unordered_map<int,int> m_researchCount;     // 科技 Action -> 已发起次数
static int m_convertTarget = -1;                        // 上次转化目标 SN
static int m_lastConvertedSN = -1;                      // 刚转化成功的那个敌人 SN
                                                        // （已变友军，但敌人快照可能残留 1~2 帧；
                                                        //   拉怪找"第二个战车弓兵"时必须排除它）
static int m_convertStartFrame = -1;                    // 上次转化下令帧
static int m_priestLastBlood = -1;                      // 祭司上一帧血量
static int m_priestMoveFrame = -9999;                   // 上次祭司移动下令帧
// 【修复·祭司打转/不转化】拉怪限时 + 撤退点锁存（见 handlePriest 里的说明）
static const int CONVERT_MAX_DIST = 10;        // 【修复·转化被打断】只转化这个距离内的敌人
                                               //   引擎：目标跑出 DIS_PRIEST(12) 格 → 施法计时清零重随机 ✗
                                               //   留 2 格余量；也避免祭司为了转化跑出塔外（"出塔迎击"）✗
static const int PRIEST_LEASH = 8;             // 【修复·出塔被围殴】祭司离最近箭塔的最大格数
                                               //   超过就立刻回塔；转化/迎击都要在塔的保护圈内做 ✓
static const int LURE_TIMEOUT_FRAMES  = 300;   // 拉怪最多持续 12 秒
static const int LURE_COOLDOWN_FRAMES = 900;   // 拉怪失败后 36 秒内不再拉怪（专心转化）
static int m_lureStartFrame = -1;              // 本次拉怪开始帧
static int m_lureCoolUntil  = 0;               // 拉怪冷却截止帧（0=从未拉过）
static int m_retGX[2] = { -1, -1 };            // 锁存的撤退点（0=拉怪 1=威胁撤退）
static int m_retGY[2] = { -1, -1 };
static int m_retGF[2] = { -1, -1 };            // 锁存点的计算帧
static double m_priestMoveDR = 0, m_priestMoveUR = 0;   // 上次祭司移动目标
static std::unordered_map<int,int> m_towerSwitch;       // 箭塔SN -> 上次下令帧
static std::unordered_map<int,int> m_armySwitch;        // 军队SN -> 上次转火帧
static int m_enemyDirX = 0, m_enemyDirY = 0;            // 敌人来袭方向（±1）
static bool m_builderGathering = false;                 // 建造者"没活干、临时采集"中

// isBadTarget 原来是头文件里的内联函数（用到 m_badTarget），随状态一起挪到文件作用域
bool UsrAI::isBadTarget(int sn, int frame) const
{
    auto it = m_badTarget.find(sn);
    return (it != m_badTarget.end()) && (frame - it->second < 600);
}

// 祭司探路参数
#define SCOUT_MAX_COUNT 20       // 探路次数上限（探完即回塔，探得更多发育更快）
#define FRAME_WAVE1     6000     // 第一波进攻帧数（约4分钟）
#define FRAME_WAVE2     13500    // 第二波进攻帧数（约9分钟）
#define FRAME_WAVE3     21000    // 第三波进攻帧数（约14分钟）
#define SCOUT_CANDIDATE 20       // 每次随机抽选的候选空地数量（越大跨度越大、扩散越快）
#define SCOUT_MAX_RANGE 30       // 探路目标距塔的最大距离（格）：探得更远，仍不脱离塔太远

// 经济发展目标
#define TARGET_FARMER_NUM 20     // 目标村民数量（市镇中心持续生产到 20 个）
#define TARGET_HOUSE_NUM 5       // 目标住房数量（上限=4+5*4=24：20农民+4兵）

// 建筑占地尺寸（按 BUILDING_TYPE 顺序：房屋2x2、箭塔2x2，其余3x3）
static const int BUILD_SIZE[BUILDING_TYPE_MAXNUM] = {
    2, 3, 3, 3, 3, 3, 2, 3, 3, 3, 3, 3, 3, 3, 3
};

// 祭司探路：贴着"探索边界"来回走，让已探明区域一圈圈向外扩散
// 原理：
//   - 只把"旁边还有未探索区域(-2)的空地"当作目标（探索边界）
//   - 角落外围是已探明的海洋(-1)，不是未探索区，所以永远不会选到角落
//   - 每走到一个边界空地，视野(12格)照亮边界外的新区域，边界随之外移
//   - 探路结束（次数用完或到第一波）后，回箭塔/市中心附近待命
void UsrAI::scoutWithPriest(const tagInfo& info)
{
    // 1) 找到祭司
    int priestSN = -1;
    const tagArmy* priest = nullptr;
    for (const tagArmy& a : info.armies) {
        if (a.Sort == AT_PRIEST) { priestSN = a.SN; priest = &a; break; }
    }
    if (priest == nullptr) return;                  // 祭司不存在（死亡=游戏失败）
    // ★round2#3 反攻期间祭司全权交给 attackPhase（与 defense/handlePriest 的让位一致）
    //   否则他会把在厂门口施法的祭司 HumanMove 回家 → suspendRelation → 转化作废 ✗
    if (m_atkOn) return;
    if (m_issued.count(priestSN)) return;           // 本帧已被其他模块下令（如避险撤退）

    // 1.5) 【修复·反复移动】场上有敌人（第一波开打/波次残兵）时祭司不探路：
    //      专心贴塔 + 转化，否则"探路目标"会和 handlePriest 的"回塔待命"互相打断。
    if (!info.enemy_armies.empty() || !info.enemy_farmers.empty()) {
        // 【修复·第一次转化失败 + 来回徘徊】有敌人时**本函数不再碰祭司**：
        //   原来这里会把他拽回"塔中心"（getPriestHome），而 handlePriest 第 3.5/5 步
        //   要他站在"塔背面"（偏移 1.5 格）——两者在 2.0 格阈值附近来回打架：
        //     · 祭司在塔背面 ≈1.5 格 + movePriest 的 1 格到达容差 → 可能 > 2.0 格
        //     · 于是本函数把他拽回塔心 → handlePriest 再推回塔背面 → **来回徘徊**
        //   更致命：施法期间快照若仍显示 IDLE（延迟 1~2 帧），这里发的 HumanMove 会
        //   suspendRelation → **把正在进行的转化整个取消**（实测"第一次转化失败"）。
        //   祭司站位已由 handlePriest 第 5 步（威胁<10格→撤到塔背面）与
        //   第 6 步（离塔>6格→回塔下待命）完整负责，这里不需要也不应该再插手。
        return;
    }

    // 2) 探路结束 → 回祭司站位（双塔中点 > 单塔 > 市中心）
    //    【修复·反复移动】提前结束时机必须与 handlePriest 第 6 步"回塔下待命"完全一致
    //    （两者都用 FRAME_WAVE1 - 2000 = 4000 帧）：原来这里是 -1500（4500），
    //    导致 4000~4500 帧之间"一个要回塔、一个要去探路边界" → 每帧交替下令 → 原地抽搐。
    if (m_scoutIdx >= SCOUT_MAX_COUNT || info.GameFrame > FRAME_WAVE1 - 2000) {
        if (m_centerX >= 0) {
            int hx, hy;
            getPriestHome(info, hx, hy);
            double homeDR = (double)hx * BLOCKSIDELENGTH;
            double homeUR = (double)hy * BLOCKSIDELENGTH;
            // 还没回到基地附近（5格内）且空闲 → 下令回家（节流下令，防每帧重复）
            if (calDistance(priest->DR, priest->UR, homeDR, homeUR) > 5.0 * BLOCKSIDELENGTH
                && priest->NowState == HUMAN_STATE_IDLE) {
                movePriest(priestSN, priest->DR, priest->UR, homeDR, homeUR, info.GameFrame);
            }
        }
        return;
    }

    // 3) 收集"探索边界空地"：已探明空地(0)，且 4 邻域存在未探索块(-2)
    //    限制：目标距塔不超过 SCOUT_MAX_RANGE 格（探路不脱离塔保护）
    static const int DX[4] = { 1, -1,  0,  0 };
    static const int DY[4] = { 0,  0,  1, -1 };
    // 找最近的塔（没有则市中心）作为探路中心
    int cxt = m_centerX, cyt = m_centerY;
    double bestT = 1e18;
    for (const tagBuilding& b : info.buildings) {
        if (b.Type != BUILDING_ARROWTOWER || b.Percent < 100) continue;
        double d = calDistance(priest->DR, priest->UR,
                               (double)b.BlockDR * BLOCKSIDELENGTH, (double)b.BlockUR * BLOCKSIDELENGTH);
        if (d < bestT) { bestT = d; cxt = b.BlockDR; cyt = b.BlockUR; }
    }
    double towerDR = (double)cxt * BLOCKSIDELENGTH;
    double towerUR = (double)cyt * BLOCKSIDELENGTH;
    std::vector<Point> frontier;
    for (const Point& p : m_explored) {
        bool hasUnknown = false;
        for (int d = 0; d < 4; ++d) {
            int nx = p.x + DX[d], ny = p.y + DY[d];
            if (nx >= 0 && nx < 100 && ny >= 0 && ny < 100 && m_map[nx][ny] == -2) {
                hasUnknown = true;
                break;
            }
        }
        if (!hasUnknown) continue;
        // 距塔太远则跳过（不脱离塔保护）
        double dt = calDistance((double)p.x * BLOCKSIDELENGTH, (double)p.y * BLOCKSIDELENGTH, towerDR, towerUR);
        if (dt > SCOUT_MAX_RANGE * BLOCKSIDELENGTH) continue;
        frontier.push_back(p);
    }
    if (frontier.empty()) return;                   // 边界没了（全图探完）

    // 4) 从边界空地随机抽候选，选离祭司最远的（边界上来回走，扩散快）
    int bestIdx = -1;
    double bestDist = -1.0;
    int tries = (frontier.size() < (size_t)SCOUT_CANDIDATE) ? (int)frontier.size() : SCOUT_CANDIDATE;
    for (int k = 0; k < tries; ++k) {
        int idx = (int)(Rand.nextRaw() % frontier.size());
        const Point& p = frontier[idx];
        double d = calDistance(priest->DR, priest->UR,
                               (double)p.x * BLOCKSIDELENGTH, (double)p.y * BLOCKSIDELENGTH);
        if (d > bestDist) { bestDist = d; bestIdx = idx; }
    }
    if (bestIdx < 0) return;
    const Point& target = frontier[bestIdx];
    double tx = (double)target.x * BLOCKSIDELENGTH;
    double ty = (double)target.y * BLOCKSIDELENGTH;

    // 5) 已到达目标（9格内）→ 本次探路完成，换新目标（阈值大 = 移动节奏快）
    if (calDistance(priest->DR, priest->UR, tx, ty) < 9.0 * BLOCKSIDELENGTH) {
        m_scoutIdx++;
        m_scoutStartFrame = -1;
        return;
    }

    // 6) 祭司空闲 → 下令前往目标（节流下令；只有真下令才记录开始帧，卡住超时才准确）
    if (priest->NowState == HUMAN_STATE_IDLE) {
        if (movePriest(priestSN, priest->DR, priest->UR, tx, ty, info.GameFrame))
            m_scoutStartFrame = info.GameFrame;
    }
    // 7) 卡住超时：下令后 120 帧（5秒）还没到达 → 换新目标（缩短超时，避免傻站）
    else if (m_scoutStartFrame >= 0 && info.GameFrame - m_scoutStartFrame > 120) {
        m_scoutIdx++;
        m_scoutStartFrame = -1;
    }
}

// ============================================================
// 侦察骑兵探路：和祭司同样的"探索边界扩散"算法
// 时机：马厩训练出侦察骑兵后自动启用，持续探索到第三波前
// 特点：视野8、速度快、牺牲不致命 → 后期远探（定位敌人位置）
// ============================================================
void UsrAI::scoutWithScout(const tagInfo& info)
{
    if (info.GameFrame > FRAME_WAVE3) return;       // 第三波后停止探路（要集中打仗）
    if (m_explored.empty()) return;                 // 还没有已探明空地

    // 找一个空闲的侦察骑兵（未被本帧其他模块下令）
    int scoutSN = -1;
    const tagArmy* scout = nullptr;
    for (const tagArmy& a : info.armies) {
        if (a.Sort != AT_SCOUT) continue;
        if (a.NowState != HUMAN_STATE_IDLE) continue;
        if (m_issued.count(a.SN)) continue;         // 已被派去攻击等
        scoutSN = a.SN;
        scout = &a;
        break;
    }
    if (scout == nullptr) return;

    // 收集"探索边界空地"（与祭司探路相同：旁边有未探索块的空地）
    static const int DX[4] = { 1, -1,  0,  0 };
    static const int DY[4] = { 0,  0,  1, -1 };
    std::vector<Point> frontier;
    for (const Point& p : m_explored) {
        bool hasUnknown = false;
        for (int d = 0; d < 4; ++d) {
            int nx = p.x + DX[d], ny = p.y + DY[d];
            if (nx >= 0 && nx < 100 && ny >= 0 && ny < 100 && m_map[nx][ny] == -2) {
                hasUnknown = true;
                break;
            }
        }
        if (hasUnknown) frontier.push_back(p);
    }
    if (frontier.empty()) return;

    // 随机抽候选，选离它最远的（大跨度扩散）
    int bestIdx = -1;
    double bestDist = -1.0;
    int tries = (frontier.size() < (size_t)SCOUT_CANDIDATE) ? (int)frontier.size() : SCOUT_CANDIDATE;
    for (int k = 0; k < tries; ++k) {
        int idx = (int)(Rand.nextRaw() % frontier.size());
        const Point& p = frontier[idx];
        double d = calDistance(scout->DR, scout->UR,
                               (double)p.x * BLOCKSIDELENGTH, (double)p.y * BLOCKSIDELENGTH);
        if (d > bestDist) { bestDist = d; bestIdx = idx; }
    }
    if (bestIdx < 0) return;
    const Point& target = frontier[bestIdx];
    double tx = (double)target.x * BLOCKSIDELENGTH;
    double ty = (double)target.y * BLOCKSIDELENGTH;

    // 到达（9格内）→ 本帧不动，下一帧自动重新随机选新目标
    if (calDistance(scout->DR, scout->UR, tx, ty) < 9.0 * BLOCKSIDELENGTH) return;

    HumanMove(scoutSN, tx, ty);
    m_issued.insert(scoutSN);
}

// ============================================================
// 建立地图标记数组（PPT 阶段1算法框架）
// 标记规则：-2=未探索，-1=海洋，0=已探明空地，>0=被占用（资源/建筑/单位）
// ============================================================
void UsrAI::updateMap(const tagInfo& info)
{
    m_explored.clear();
    if (info.theMap == nullptr) return;
    const auto& terrain = *info.theMap;

    // 1) 从 *theMap 读取地形：未探索/海洋标记为负数，已探明陆地标记为 0
    for (int i = 0; i < 100; ++i) {
        for (int j = 0; j < 100; ++j) {
            int t = terrain[i][j].type;
            if (t == MAPPATTERN_UNKNOWN) {
                m_map[i][j] = -2;       // 未探索
            } else if (t == MAPPATTERN_OCEAN) {
                m_map[i][j] = -1;       // 海洋（水面）
            } else {
                m_map[i][j] = 0;        // 已探明陆地（空地）
            }
        }
    }

    // 2) 遍历资源，标记为资源标号（10+资源类型）
    for (const tagResource& r : info.resources) {
        if (r.BlockDR >= 0 && r.BlockDR < 100 && r.BlockUR >= 0 && r.BlockUR < 100)
            m_map[r.BlockDR][r.BlockUR] = 10 + r.Type;
    }

    // 3) 遍历建筑，按占地尺寸标记为建筑标号（100+建筑类型）
    for (const tagBuilding& b : info.buildings)
        markBlock(b.BlockDR, b.BlockUR, BUILD_SIZE[b.Type % BUILDING_TYPE_MAXNUM], 100 + b.Type);
    for (const tagBuilding& b : info.enemy_buildings)
        markBlock(b.BlockDR, b.BlockUR, BUILD_SIZE[b.Type % BUILDING_TYPE_MAXNUM], 100 + b.Type);

    // 4) 遍历单位，标记为单位标号（200=我方 300=敌方）
    for (const tagFarmer& f : info.farmers)       markBlock(f.BlockDR, f.BlockUR, 1, 200);
    for (const tagArmy&   a : info.armies)        markBlock(a.BlockDR, a.BlockUR, 1, 200);
    for (const tagFarmer& f : info.enemy_farmers) markBlock(f.BlockDR, f.BlockUR, 1, 300);
    for (const tagArmy&   a : info.enemy_armies)  markBlock(a.BlockDR, a.BlockUR, 1, 300);

    // 5) 收集已探明的空地（m_map == 0 的块，未被占用、可通行）
    for (int i = 0; i < 100; ++i) {
        for (int j = 0; j < 100; ++j) {
            if (m_map[i][j] == 0) m_explored.push_back(Point(i, j));
        }
    }
}

// 在 m_map 上标记一片 w×w 占用区域（只覆盖空地）
void UsrAI::markBlock(int bx, int by, int size, int val)
{
    for (int i = 0; i < size; ++i) {
        for (int j = 0; j < size; ++j) {
            int x = bx + i, y = by + j;
            if (x >= 0 && x < 100 && y >= 0 && y < 100 && m_map[x][y] == 0)
                m_map[x][y] = val;
        }
    }
}

// ============================================================
// 寻找 w×h 的可建造空地（返回左下角块坐标）
// 要求：区域内全是已探明空地(m_map==0)，且高度一致（平地）
// (i,j) 处能否放 w×h 建筑：地图内 + m_map 空闲 + 地形等高 + 不紧挨浆果丛
// ============================================================
bool UsrAI::canPlace(const tagInfo& info, int i, int j, int w, int h) const
{
    if (info.theMap == nullptr) return false;
    const auto& terrain = *info.theMap;
    if (i < 0 || j < 0 || i + w > 100 || j + h > 100) return false;
    int height = terrain[i][j].height;
    for (int di = 0; di < w; ++di) {
        for (int dj = 0; dj < h; ++dj) {
            if (m_map[i + di][j + dj] != 0) return false;
            if (terrain[i + di][j + dj].height != height) return false;
        }
    }
    // 外扩 1 圈避开浆果丛：建筑不能紧挨采集点，否则农民采浆果会被卡住
    for (int di = -1; di <= w; ++di) {
        for (int dj = -1; dj <= h; ++dj) {
            if (di >= 0 && di < w && dj >= 0 && dj < h) continue;   // 跳过建筑内部
            int nx = i + di, ny = j + dj;
            if (nx < 0 || nx >= 100 || ny < 0 || ny >= 100) continue;
            if (m_map[nx][ny] == 10 + RESOURCE_BUSH) return false;
        }
    }
    return true;
}

// 以 (cx,cy) 为中心、由近到远（半径 0..maxR）找 w×h 空地 → 真正意义上的"建在附近"
// 【修正背景】原 findBuildBlock 只把 nearX/nearY 当"行优先扫描起点"：
//   猎物堆/浆果丛周围被树和动物占满时，它会顺着行优先一路扫到几十格以外
//   → 实测"新建的仓库都不在羚羊堆附近"。现在先在目标点附近找，找不到才由调用方决定是否放弃
// ============================================================
bool UsrAI::findBuildBlockNear(const tagInfo& info, int& x, int& y, int w, int h, int cx, int cy, int maxR)
{
    for (int r = 0; r <= maxR; ++r) {
        for (int i = cx - r; i <= cx + r; ++i) {
            for (int j = cy - r; j <= cy + r; ++j) {
                if (r > 0 && abs(i - cx) != r && abs(j - cy) != r) continue;   // 只看本圈
                if (canPlace(info, i, j, w, h)) { x = i; y = j; return true; }
            }
        }
    }
    return false;
}

// ============================================================
// 全局找可建空地（不关心位置）：起点 = 指定附近位置 > 缓存的搜索位置 > 市镇中心附近
//   nearX/nearY >= 0：先在该点附近 12 格内由近到远找；找不到再退回全局扫描（保底能建出来）
// ============================================================
bool UsrAI::findBuildBlock(const tagInfo& info, int& x, int& y, int w, int h, int nearX, int nearY)
{
    if (info.theMap == nullptr) return false;

    if (nearX >= 0 && nearY >= 0) {
        if (findBuildBlockNear(info, x, y, w, h, nearX, nearY, 12)) {
            m_searchX = x;
            m_searchY = (y + 1 < 100 ? y + 1 : 0);
            return true;
        }
    }

    int startX = m_searchX, startY = m_searchY;
    if (m_centerX > 0 && startX == 0 && startY == 0) {
        startX = (m_centerX > 8 ? m_centerX - 8 : 0);
        startY = (m_centerY > 8 ? m_centerY - 8 : 0);
    }

    // 两遍扫描：第一遍从起点开始，第二遍从头开始（覆盖起点之前的区域）
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = (pass == 0 ? startX : 0); i < 100; ++i) {
            for (int j = (pass == 0 && i == startX ? startY : 0); j < 100; ++j) {
                if (canPlace(info, i, j, w, h)) {
                    x = i; y = j;
                    // 下次搜索从当前位置旁边继续，避免反复找到同一块地
                    m_searchX = i;
                    m_searchY = (j + 1 < 100 ? j + 1 : 0);
                    return true;
                }
            }
        }
    }
    return false;
}

// ============================================================
// 农民工作分配（铜器时代前的动态最优方案）
// 核心规则：采粮食的人始终 >= 总人数一半
//   ① 浆果(开局4人，采完自动转) → ② 打猎(高效) → ③ 种田(持续)
//   木头≈1/4、石头1人、铜器后黄金3人、建房1人
// ============================================================
// 【编译修复·前置声明】resSafe 的**定义**在文件靠后（约 :1083），
//   但 manageVillagers 里"后期调用伐木富余人口打大象"的逻辑（约 :600）要先用它 ✗
//   → 在这里先声明、后面再定义 ✓（C++ 允许先声明后定义）
static bool resSafe(const tagInfo& info, const tagResource& r);

void UsrAI::manageVillagers(const tagInfo& info)
{
    farmerDeathWatch(info);          // 【用户要求】记录农民位置 + 远处阵亡点标记 ✓
    // 1) 统计当前各工种人数（通过工作对象 SN 查类型）
    int total = 0, foodCnt = 0, berryCnt = 0, woodCnt = 0, stoneCnt = 0, goldCnt = 0, huntCnt = 0;
    bool berryExists = false;
    for (const tagResource& r : info.resources)
        if (r.Type == RESOURCE_BUSH && r.Cnt > 0) berryExists = true;

    for (const tagFarmer& f : info.farmers) {
        if (f.FarmerSort != FARMERTYPE_FARMER) continue;
        total++;
        // 【关键修复】在途(WALKING)农民也必须计入配额！
        //   原来只统计 WORKING：农民走在路上时不计入 → 下一个空闲农民又被派去同一工种
        //   → 配额被反复突破（实测"很多农民去伐木"的根因）
        if (f.NowState != HUMAN_STATE_WORKING && f.NowState != HUMAN_STATE_WALKING) continue;
        // 工作对象是农田？
        bool isFarm = false;
        for (const tagBuilding& b : info.buildings)
            if (b.SN == f.WorkObjectSN && b.Type == BUILDING_FARM) { isFarm = true; break; }
        if (isFarm) { foodCnt++; continue; }
        // 工作对象是资源？
        for (const tagResource& r : info.resources) {
            if (r.SN != f.WorkObjectSN) continue;
            if (r.Cnt <= 0) break;                    // 目标已采完（浆果/动物尸体等），不算有效工作
            switch (r.Type) {
            case RESOURCE_BUSH:   berryCnt++; foodCnt++; break;
            case RESOURCE_GAZELLE:
            case RESOURCE_ELEPHANT:
            case RESOURCE_LION:
            case RESOURCE_FISH:   foodCnt++; if (r.Type != RESOURCE_FISH) huntCnt++; break;
            case RESOURCE_TREE:   woodCnt++; break;
            case RESOURCE_STONE:  stoneCnt++; break;
            case RESOURCE_GOLD:   goldCnt++; break;
            default: break;
            }
            break;
        }
    }

    // 2) 统计每个工作目标被几个农民使用（用于避免资源点扎堆）
    //    【修复】只统计"真正在用该目标"的农民（工作 + 在途）；原逻辑统计"非工作"农民会误算
    std::unordered_map<int,int> targetCount;
    for (const tagFarmer& f : info.farmers)
        if ((f.NowState == HUMAN_STATE_WORKING || f.NowState == HUMAN_STATE_WALKING)
            && f.WorkObjectSN > 0)
            targetCount[f.WorkObjectSN]++;

    // 3) 动态配额【3.0.7g 发育策略】
    //    开局：4 采果 + 3 伐木 + 1 专职建造（正好 8 人，不挖石：初始 150 石正好建 1 座塔）
    bool bronze = (info.civilizationStage >= CIVILIZATION_BRONZEAGE);
    int targetFood = total / 2;                 // 采粮人数 >= 总人数一半
    if (targetFood < 5) targetFood = 5;         // 保底 5 个
    int targetWood = 3;                         // 开局 3 伐木（策略指定）
    // 【发育策略】木材不够就派 1~2 人帮忙伐木（3 → 4 → 5，最多 5 人）
    //   原逻辑只在"升级建筑尚未建成"时加人 → 铜器后木头不够不会加人，与策略不符
    // 【ai_log 实证】f=6011 时木头只有 55 ✗ → 升级链（市场+兵营+靶场=425 木）彻底卡住 ✓
    //   文档 71 行也说"木材会成为瓶颈，怎么都不太够" → 前期伐木上限 5 → 8 ✓
    if (info.Wood < 150) targetWood = 6;        // 木头不足：6 人
    if (info.Wood < 80)  targetWood = 8;        // 严重不足：8 人
    // 【发育策略】不派挖石工：初始 150 石正好建 1 座塔（塔上限 1 座），人力全给食物/木头/黄金
    int targetStone = 0;
    // 【3.0.7g 调整】金矿 200→400（翻倍）→ 黄金更充裕，挖金保持 3 人
    // 【发育策略】3 人采金：采金不占食物预算，且铜器后造兵急用黄金；多余农民优先采金而非伐木
    // 【用户要求·3.0.7g】升级铜器前**不采黄金**：原来 3 个采金的人先去采食物/木材；
    //   一旦铜器升级已经开始（市中心正在升级，TIME_BUILDING_CENTER_UPGRADE=60 秒）
    //   → 按升级进度"陆续"派人去采金：0 人 → 1 → 2 → 3 人；升完铜器后固定 3 人。
    int targetGold = 3;
    if (info.civilizationStage < CIVILIZATION_BRONZEAGE) {
        // 【策略文档 71 行·早期采金】"安排村民早点采集黄金，免得技术升好了没资源造兵"
        //   原来升级前 0 人采金 ✗ → ai_log 实证 f=5411 黄金仍是 0 ✗
        //   而复合弓兵要 20 金/个 ✗ → 没金 = 造不出兵 ✓
        if ((int)info.farmers.size() >= 10) targetGold = 2;
        if (m_bronzeUpgradeFrame >= 0) {
            int elapsed = info.GameFrame - m_bronzeUpgradeFrame;
            targetGold = 2 + elapsed / 500;        // 升级中再陆续加人（每 20 秒 +1）
            if (targetGold > 3) targetGold = 3;
        }
    }

    // ===== 【用户要求·铜器后人员配额】=====
    //   采金 5 人（固定）；木材 4 人，木材不够(<150)时临时加到 6 人；
    //   其余**全部采集食物**——食物是造兵/科技的唯一瓶颈（实测食物只有 10~28 时
    //   180 食的复合弓科技和造兵全卡住，而黄金却堆到 260）。
    // ===== 【修复·经济崩盘：金 450 / 木 20 / 食 45】铜器后改成"按库存缺口"分配 =====
    //   旧逻辑：采金**固定 5 人**、木材只给 4（<150 才 6）、其余食物
    //   → 黄金严重过剩（实测堆到 450，够造 5 个骑兵），而木头只有 4~6 人在砍 ✗
    //   木头是**最大消耗口**：农田 75 木/块、被采空删除后还要反复重建
    //   （8 块田≈每 2 分钟 600 木），再加建筑 120~180 木/座 → 必然见底
    //   → 农田建不出 → 食物跟着崩 → 没兵、连 50 食的箭塔科技都研不了 ✗
    //   （课程文档也写过："如果种田的话，木材会成为瓶颈，怎么都不太够"）
    //   现在：木头按缺口给足人；黄金够用就撤人（多采毫无意义）
    if (bronze) {
        // 【修复·复合弓科技卡住】科技要 180 食 + 100 木，而它的前置靶场还要
        //   市场(150木)+兵营(125木)+靶场(150木)=425 木 ✗ —— 合计木 525 / 食 980 ✓
        //   靶场建成**之前**，黄金一点用都没有（这三个建筑都不要金 ✗）
        //   → 所以这个阶段：伐木拉到 10 人、采金 0 人，其余食物 ✓
        if (countBuilding(info, BUILDING_RANGE) == 0) {      // 靶场还没建成
            targetWood = (total >= 14) ? 10 : 6;             // 全力伐木
            targetGold = 0;                                  // ★不采金（黄金此刻没用）
            targetFood = total - targetWood - targetGold;
            if (targetFood < 4) targetFood = 4;
        } else if (info.Wood < 80)        targetWood = 10;   // 木头见底 → 全力伐木
        else if (info.Wood < 200)  targetWood = 8;
        else                       targetWood = 5;
        // 【用户要求】采金人数**固定**：默认 GOLD_FARMERS_FIX 人；黄金堆多（≥300）只留 2 人
        // 【修复·顺序】靶场没建成 → 不采金（黄金此刻毫无用途，别浪费人力 ✗）
        targetGold = (countBuilding(info, BUILDING_RANGE) == 0)
                     ? 0
                     : ((info.Gold >= 300) ? 2 : GOLD_FARMERS_FIX);
        targetFood = total - targetWood - targetGold; // 其余全采食物
        if (targetFood < 6) targetFood = 6;
    }

    // ===== 【用户建议·大象留到后期，一次性调用多余的伐木工】=====
    //   ① 后期才做 ② 有安全且活着的大象 ③ 伐木工有富余 ④ 同一帧能凑够 3 人才动手
    //   注意：这里是**主动把正在伐木的人改派去打猎**（不是等他们空闲 ✗ —— 伐木工永远不空闲）
    if (bronze && info.GameFrame > ELE_CALL_FRAME) {
        int eleSN = -1;
        for (const tagResource& r : info.resources) {
            if (r.Type != RESOURCE_ELEPHANT || r.Blood <= 0 || r.Cnt <= 0) continue;
            if (!resSafe(info, r)) continue;         // 只打安全的（离敌营远、离基地不远 ✓）
            eleSN = r.SN;
            break;
        }
        if (eleSN >= 0 && eleSN != m_eleCallSN) {
            int woodN = 0;                            // 实际伐木人数
            int onEle = 0;                            // 已经在这头大象身上的人数
            int avail = 0;                            // 本帧能派过去的伐木工数
            for (const tagFarmer& w : info.farmers) {
                if (w.WorkObjectSN == eleSN
                    && (w.NowState == HUMAN_STATE_WORKING || w.NowState == HUMAN_STATE_WALKING)) ++onEle;
                if (!(m_role.count(w.SN) && m_role[w.SN] == 2)) continue;   // 只看伐木工
                if (w.NowState == HUMAN_STATE_WORKING || w.NowState == HUMAN_STATE_WALKING) ++woodN;
                if (!m_issued.count(w.SN)
                    && (w.NowState == HUMAN_STATE_WORKING || w.NowState == HUMAN_STATE_WALKING)) ++avail;
            }
            // ③ 富余判定：伐木人数超过目标（targetWood）的部分才算"多余人口" ✓
            const int surplus = woodN - targetWood;
            // ④ 同一帧能凑够 ELE_CALL_TEAM 人才动手（不够就等下一帧，绝不零散送死 ✓）
            if (onEle == 0 && surplus >= ELE_CALL_TEAM && avail >= ELE_CALL_TEAM) {
                int sent = 0;
                for (const tagFarmer& w : info.farmers) {
                    if (sent >= ELE_CALL_TEAM) break;
                    if (m_issued.count(w.SN)) continue;
                    if (!(m_role.count(w.SN) && m_role[w.SN] == 2)) continue;
                    if (w.NowState != HUMAN_STATE_WORKING && w.NowState != HUMAN_STATE_WALKING) continue;
                    HumanAction(w.SN, eleSN);         // ★同一帧连续 3 条令 → 三人同时开打 ✓
                    m_issued.insert(w.SN);
                    m_role[w.SN] = 4;                 // 工种改成打猎（打完可以去种田 ✓）
                    ++sent;
                }
                if (sent >= ELE_CALL_TEAM) m_eleCallSN = eleSN;   // 成队才算调用过 ✓
            }
        }
    }

    // 3) 逐个给空闲农民分配工作
    //    额外处理：非空闲但"工作目标失效"的农民（如猎取的羚羊尸体已被采完）
    //    也重新分配，避免卡在无效目标上不动
    for (const tagFarmer& f : info.farmers) {
        if (f.FarmerSort != FARMERTYPE_FARMER) continue;
        if (f.SN == m_builderSN) continue;              // 专职建造者不参与采集（由 buildBuildings 调度）
        // 【修复·谷仓没人建】资源点仓库/谷仓的专职建造者（m_depotBuilderSN）同样不能被抢走：
        //   这里原来只排除了 m_builderSN。于是 buildResourceDepots 刚下完建造令，下一帧本函数
        //   就把他改派去采集 → HumanAction 会取消正在进行的 CoreEven_FixBuilding（建造），
        //   地基建了一半再没人管，而 AI 还会在旁边再下一个 → 同一处浆果两个地基都没人建。
        if (f.SN == m_depotBuilderSN) continue;
        if (m_issued.count(f.SN)) continue;
        bool isFood = (m_foodGatherers.count(f.SN) > 0) && !bronze;
        //  专属食物采集者（浆果/打猎）：只做食物（浆果→打猎→种田）
        //  升级铜器后专属标记失效 → 除专职建造工外所有农民重新分配任务（挖金/砍树/种田）

        // 非空闲农民：检查工作目标是否仍然有效（存在且有剩余），并检测寻路卡住
        if (f.NowState != HUMAN_STATE_IDLE) {
            // 【用户要求】正在采集农田的农民：这块田快采完了（剩余 ≤40）而田还不够
            //   → **他自己去建一块新田**（不等引擎删田、也不用等专职建造者）→ 食物链不断档
            if (f.NowState == HUMAN_STATE_WORKING
                && m_role.count(f.SN) && m_role[f.SN] == 5) {
                const tagBuilding* myFarm = nullptr;
                for (const tagBuilding& fb : info.buildings)
                    if (fb.SN == f.WorkObjectSN && fb.Type == BUILDING_FARM) { myFarm = &fb; break; }
                if (myFarm != nullptr && myFarm->Cnt <= 40
                    && (!mapFoodLeft(info) || info.Meat < 200)) {
                    int wantR = (info.GameFrame > FRAME_WAVE2) ? ((int)info.farmers.size() / 2) : 3;
                    if (wantR < FARM_MIN_COUNT) wantR = FARM_MIN_COUNT;
                    if (wantR > FARM_MAX_COUNT) wantR = FARM_MAX_COUNT;
                    if (countBuilding(info, BUILDING_FARM) < wantR
                        && info.Wood >= ((info.Meat < 150) ? 80 : FARM_WOOD_GATE)) {   // ★食物告急→门槛放宽到 80，避免死锁
                        int gxp = m_centerX, gyp = m_centerY;
                        for (const tagBuilding& b : info.buildings)
                            if (b.Type == BUILDING_GRANARY) { gxp = b.BlockDR; gyp = b.BlockUR; break; }
                        int xp, yp;
                        if (findBuildBlock(info, xp, yp, 3, 3, gxp, gyp)) {
                            HumanBuild(f.SN, BUILDING_FARM, xp, yp);
                            m_issued.insert(f.SN);
                            continue;               // 他自己去建新田
                        }
                    }
                }
            }
            bool valid = false;
            for (const tagResource& r : info.resources)
                if (r.SN == f.WorkObjectSN && r.Cnt > 0) { valid = true; break; }
            if (!valid) {
                for (const tagBuilding& b : info.buildings)
                    if (b.SN == f.WorkObjectSN) { valid = true; break; }   // 农田等建筑目标
            }
            if (valid) {
                // 卡住检测【3.0.7g 加强】：某些地图树/矿在水边或被挡住 → 农民原地罚站
                //   a) 走路：近距(<15格) 动物 60 帧 / 静态资源 120 帧；中距(<28格) 静态资源 400 帧
                //   b) 已在"工作"却没站到目标旁(>3格) → 引擎寻路失败被卡住
                //   判定卡住 → 该目标拉黑 600 帧（换一棵树/一处矿），不再反复重派同一目标
                bool isAnimal = false;
                double targetDR = 0, targetUR = 0;
                bool hasTarget = false;
                for (const tagResource& r : info.resources) {
                    if (r.SN != f.WorkObjectSN) continue;
                    if (r.Type == RESOURCE_GAZELLE || r.Type == RESOURCE_ELEPHANT || r.Type == RESOURCE_LION)
                        isAnimal = true;
                    targetDR = r.DR;
                    targetUR = r.UR;
                    hasTarget = true;
                    break;
                }
                bool stuck = false;
                bool suspect = false;              // 是否处于"可疑计时中"（计时存在 m_moveStart 里）
                // 【修复·关键】卡住判定必须看"有没有在靠近"：
                //   原先只看"状态 + 距离 + 帧数"，村民走向远处的树时状态已是 WORKING，
                //   100 帧后就被当成卡住 → 反复改目标 → 界面一直刷"设置工作目标为 树 X"。
                //   现在：距离在缩小 = 有进展（重置计时）；只有"原地不动"累计到超时才判卡住。
                if (hasTarget) {
                    double tDist = calDistance(f.DR, f.UR, targetDR, targetUR);
                    bool inCooldown = false;
                    {
                        auto cf = m_stuckFrame.find(f.SN);
                        if (cf != m_stuckFrame.end() && info.GameFrame - cf->second < 300)
                            inCooldown = true;     // 刚判过他卡住 → 300 帧内不再判，给他时间走过去
                    }
                    int timeout = 0;
                    if (!inCooldown && f.NowState == HUMAN_STATE_WALKING) {
                        if (tDist < 15.0 * BLOCKSIDELENGTH) timeout = isAnimal ? 60 : 120;
                        else if (!isAnimal && tDist < 28.0 * BLOCKSIDELENGTH) timeout = 400;
                    } else if (!inCooldown && f.NowState == HUMAN_STATE_WORKING
                               && !isAnimal && tDist > 3.0 * BLOCKSIDELENGTH) {
                        timeout = 150;             // 说在干活却离资源很远
                    }
                    // 【修复·1b】身上背着货(Resource>0) → 他要么正在采集、要么正在回城卸货，
                    //   此时"到资源点的距离"本来就可能变大（回程必然变大）→ 一律不判卡、不拉黑。
                    //   ★ 这里只是不让 suspect 置位（等价于"这农民没事、别碰他"，与 timeout==0 时
                    //     既有的 continue 完全同类），**结构上不可能触碰后面的重新分配路径**。
                    //     对比 patch36 的教训：那次是在重新分配路径入口加 continue 跳过整条
                    //     优先级链 → 农民永久冻结（实测"采集资源的人都卡住了"）。
                    if (timeout > 0 && f.Resource <= 0) {
                        suspect = true;
                        auto it = m_moveStart.find(f.SN);
                        auto id = m_lastDist.find(f.SN);
                        if (it == m_moveStart.end() || id == m_lastDist.end()) {
                            m_moveStart[f.SN] = info.GameFrame;
                            m_lastDist[f.SN] = tDist;
                        } else if (tDist < id->second - 1.0 * BLOCKSIDELENGTH) {
                            // 【修复·1a】"有进展"按**整段窗口**判断，绝不能逐帧比：
                            //   农民速度 HUMAN_SPEED = 2.236 像素/帧 = 0.0625 格/帧，
                            //   原来"比上一帧靠近 0.2 格(7.16 像素)"才算进展 → 永远不成立
                            //   → 任何 3 格以外的目标满 timeout 帧必判"卡住" → 农民半路折返。
                            //   现在：整段窗口净靠近 1 格以上才算有进展（约 16 帧一次）。
                            m_moveStart[f.SN] = info.GameFrame;   // 有进展 → 重置窗口
                            m_lastDist[f.SN] = tDist;
                        } else {
                            // 【关键】不再每帧刷新 m_lastDist！否则"窗口起点"被抹掉，
                            //   计时永远从第一帧起算 → 计时器形同虚设。
                            if (info.GameFrame - it->second > timeout) stuck = true;
                        }
                    }
                }
                if (!suspect) { m_moveStart.erase(f.SN); m_lastDist.erase(f.SN); continue; }
                if (!stuck) continue;                                  // 还在计时 → 继续观察
                m_moveStart.erase(f.SN);
                m_lastDist.erase(f.SN);
                m_stuckFrame[f.SN] = info.GameFrame;                   // 冷却 300 帧，防反复改目标
                if (hasTarget) m_badTarget[f.WorkObjectSN] = info.GameFrame;   // 拉黑，换目标
            }
            // 目标失效或卡住 → 掉下去重新分配（新指令覆盖旧目标）
        }

        // 【防重复下令·关键】覆盖"所有即将重新分配"的农民（IDLE、目标已采完、判卡住三种）
        //   实测现象：农民 41226 每帧重发"设置工作目标为 树 50997"（界面/日志刷屏）。
        //   原因：引擎没接受这条采集指令（目标不可达/被挡住）时，农民位置和状态都不变，
        //         而目标一直被判定为无效 → 每帧掉进优先级链重新派树。
        //   判据：上次下令后他有没有挪动过（m_orderX/m_orderY 记录下单时的位置）。
        //     · 动过   → 指令生效了（目标被采完等正常情况）→ 立刻允许重新分配
        //     · 没动过 + 距下令 < 120 帧 → 指令被忽略 → 本帧不重复下令
        //     · 没动过 + 已超 120 帧     → 上次那个目标不可达 → 拉黑换一个，再给他一次机会
        {
            // 【脱困中】正在强制回家重置寻路的农民：给 180 帧走过去，期间不重新分配
            auto rf = m_recoverFrame.find(f.SN);
            if (rf != m_recoverFrame.end()) {
                if (info.GameFrame - rf->second < 180) continue;
                m_recoverFrame.erase(f.SN);
            }
            auto of = m_orderFrame.find(f.SN);
            if (of != m_orderFrame.end()) {
                auto ox = m_orderX.find(f.SN);
                auto oy = m_orderY.find(f.SN);
                bool moved = false;
                if (ox != m_orderX.end() && oy != m_orderY.end())
                    moved = (fabs(f.DR - ox->second) > 0.1 * BLOCKSIDELENGTH
                             || fabs(f.UR - oy->second) > 0.1 * BLOCKSIDELENGTH);
                if (moved) {                          // 指令生效 → 清掉记录，正常重新分配
                    m_orderFrame.erase(f.SN);
                    m_orderX.erase(f.SN);
                    m_orderY.erase(f.SN);
                } else if (info.GameFrame - of->second < 120) {
                    continue;                         // 刚下令还没动 → 本帧不打扰
                } else {
                    auto ot = m_orderTarget.find(f.SN);
                    if (ot != m_orderTarget.end() && ot->second > 0)
                        m_badTarget[ot->second] = info.GameFrame;   // 不可达 → 拉黑换目标
                    m_orderFrame.erase(f.SN);
                    m_orderX.erase(f.SN);
                    m_orderY.erase(f.SN);
                    // 【脱困】下令 120 帧他一步没动 → 引擎没执行这条采集指令（多半卡在
                    //   WORKING 状态的死目标上）。AI 接口没有"取消"指令，但 HumanMove 内部
                    //   会 suspendRelation()+initAction()，能把卡住的行动链整条重置。
                    //   所以先让他走回市中心，走起来后再由正常逻辑重新分配工作。
                    if (m_centerX > 0 && m_centerY > 0) {
                        HumanMove(f.SN, (double)m_centerX * BLOCKSIDELENGTH,
                                        (double)m_centerY * BLOCKSIDELENGTH);
                        m_issued.insert(f.SN);
                        m_recoverFrame[f.SN] = info.GameFrame;
                        continue;                 // 本帧只下移动令 → 走起来后下帧再重新派活
                    }
                }
            }
        }

        // 【修复·关键】重新分配前先"留在原工种"（实测：伐木的人到后面只剩一个）
        //   原因：伐木工的树采完后落入 ①浆果（优先级最高）→ 被登记成专属食物采集者，
        //         从此不再伐木；采金同理会被浆果吸走。这里先按原工种补位，再走原优先级链。
        int prevRole = 0;
        {
            auto rit = m_role.find(f.SN);
            if (rit != m_role.end()) prevRole = rit->second;
        }
        if (prevRole == 2 && woodCnt < targetWood) {          // 原来是伐木工 → 继续伐木
            int ksn = findNearestTree(info, f.SN);
            if (ksn >= 0) {
                HumanAction(f.SN, ksn);
                m_issued.insert(f.SN);
                m_orderTarget[f.SN] = ksn; m_orderFrame[f.SN] = info.GameFrame;
                m_orderX[f.SN] = f.DR; m_orderY[f.SN] = f.UR;
                woodCnt++;
                continue;
            }
        }
        if (prevRole == 3 && goldCnt < targetGold) {          // 原来是采金工 → 继续采金
            int gsn = findNearestResource(info, RESOURCE_GOLD, f.SN);
            if (gsn >= 0) {
                HumanAction(f.SN, gsn);
                m_issued.insert(f.SN);
                m_orderTarget[f.SN] = gsn; m_orderFrame[f.SN] = info.GameFrame;
                m_orderX[f.SN] = f.DR; m_orderY[f.SN] = f.UR;
                goldCnt++;
                continue;
            }
        }

        // ① 浆果：开局 4 人（配置：4浆果+2砍树+1挖石+1建造），农民增多后升到 8（第一波前新增 4 人采浆果）
        //    采浆果的农民标记为专属食物采集者（浆果采完自动找下一个食物资源）
        //    【3.0.7g 修正】原固定上限 8 会把开局全部农民吸去采浆果 → 没人砍树/挖石
        // 【发育策略】浆果最多 6 人（开局 4 人 + 新生成的 2 人去采果），之后新农民转打猎/采集
        int berryCap = ((int)info.farmers.size() <= 8) ? 4 : 6;
        if (berryExists && berryCnt < berryCap) {
            int bestSn = -1;
            int bestCnt = 1e9;
            for (const tagResource& r : info.resources) {
                if (r.Type != RESOURCE_BUSH || r.Cnt <= 0) continue;
                int c = targetCount[r.SN];
                if (c < bestCnt) { bestCnt = c; bestSn = r.SN; }
            }
            if (bestSn >= 0) {
                HumanAction(f.SN, bestSn);
                m_issued.insert(f.SN);
                m_orderTarget[f.SN] = bestSn; m_orderFrame[f.SN] = info.GameFrame;
                m_orderX[f.SN] = f.DR; m_orderY[f.SN] = f.UR;
                m_foodGatherers.insert(f.SN);   // 标记专属食物采集
                m_role[f.SN] = 1;                // 工种：浆果
                berryCnt++;
                foodCnt++;
                continue;
            }
        }
        // ② 木头（正常2人，按需动态）；专属食物采集者不砍树（只做食物）
        //    分散选树：避免两个樵夫扎堆同一棵/相邻树互相卡住
        if (!isFood && woodCnt < targetWood) {
            int sn = findNearestTree(info, f.SN);
            if (sn >= 0) {
                HumanAction(f.SN, sn);
                m_issued.insert(f.SN);
                m_orderTarget[f.SN] = sn; m_orderFrame[f.SN] = info.GameFrame;
                m_orderX[f.SN] = f.DR; m_orderY[f.SN] = f.UR;
                m_role[f.SN] = 2;                // 工种：伐木（采完树后优先回来伐木）
                woodCnt++;
                continue;
            }
        }
        // ③ 石头（1人）；专属食物采集者不挖石
        if (!isFood && stoneCnt < targetStone) {
            int sn = findNearestResource(info, RESOURCE_STONE, f.SN);
            if (sn >= 0) {
                HumanAction(f.SN, sn);
                m_issued.insert(f.SN);
                m_orderTarget[f.SN] = sn; m_orderFrame[f.SN] = info.GameFrame;
                m_orderX[f.SN] = f.DR; m_orderY[f.SN] = f.UR;
                stoneCnt++;
                continue;
            }
        }
        // ④ 打猎 → 种田 → 建农田
        //    专属食物采集者无条件做食物（浆果采完/猎物打完自动流转找下一个食物）；
        //    未升级时：所有空闲农民都优先采食物（尽快采完地图食物，不跑去砍树）；
        //    升级后：普通农民只在食物缺口时补位打猎（打猎也标记为专属，两两一组分散猎杀）
        //    【用户要求·第二波后】猎物/浆果都采光了 → "打猎的人"没猎物可打就杵着不动。
        //      现在第二波之后一律允许进入本分支：只要有空田（一田一人）就去种田，
        //      不再受"食物人数只占一半(foodCnt < targetFood)"的限制。
        const bool wave2Farm = (info.GameFrame > FRAME_WAVE2);
        if (isFood || !bronze || foodCnt < targetFood || wave2Farm) {
            int sn = findNearestHunt(info, f.SN);
            if (sn >= 0) {
                HumanAction(f.SN, sn);
                m_issued.insert(f.SN);
                m_orderTarget[f.SN] = sn; m_orderFrame[f.SN] = info.GameFrame;
                m_orderX[f.SN] = f.DR; m_orderY[f.SN] = f.UR;
                m_foodGatherers.insert(f.SN);   // 打猎也标记专属
                m_role[f.SN] = 4;                // 工种：打猎
                foodCnt++;
                huntCnt++;
                continue;
            }
            // 【发育策略·关键】想打猎但缺搭档 → 本帧原地不动，等第二个农民生成后一起派
            //   （否则这个农民会落入下面的"砍树"兜底 → 打猎人被拉去伐木、永远凑不成一对）
            // 第二波后不再"等搭档"（猎物本来就少，等不到就是永远站着）→ 直接落到种田分支
            // 【用户要求·别站着不动】等队友（配对打猎 / 大象凑 3 人）不能无限等 ✗
            //   超过 HUNT_WAIT_MAX（24 秒）还等不到 → 放弃等待，落到下面去干别的（种田/伐木 ✓）
            if (m_huntWaiting && !wave2Farm) {
                std::map<int,int>::iterator wit = m_huntWaitSince.find(f.SN);
                if (wit == m_huntWaitSince.end()) {
                    m_huntWaitSince[f.SN] = info.GameFrame;      // 开始等
                    continue;
                }
                if (info.GameFrame - wit->second < HUNT_WAIT_MAX) continue;   // 还在等
                m_huntWaitSince.erase(wit);                      // ★等太久 → 不等了，往下走
            } else {
                m_huntWaitSince.erase(f.SN);                     // 没在等 → 清记录
            }
            // 【发育策略】浆果/猎物采完后即可开田（不必等铜器）：市场已建 + 浆果已采完
            //   一片农田一个农民（findNearestFarm 就近派活，农田数量上限=采粮目标数）
            bool marketBuilt = (countBuilding(info, BUILDING_MARKET) > 0);
            bool canFarm = marketBuilt
                           && (info.civilizationStage >= CIVILIZATION_BRONZEAGE || !berryExists);
            if (canFarm) {
                // 【发育策略】一块农田一个农民：只选"当前没有农民"的农田（就近）
                int farmSN = -1;
                double bestFarmD = 1e18;
                for (const tagBuilding& fb : info.buildings) {
                    if (fb.Type != BUILDING_FARM || fb.Percent < 100) continue;
                    // 【修复】农田已采空（Cnt<=0，引擎本帧就删它）→ 派过去会被当成"修理农田"
                    //   （Core.cpp:1030-1037），农民傻站着采不到东西
                    if (fb.Cnt <= 0) continue;
                    int users = 0;
                    for (const tagFarmer& w : info.farmers)
                        if ((w.NowState == HUMAN_STATE_WORKING || w.NowState == HUMAN_STATE_WALKING)
                            && w.WorkObjectSN == fb.SN) users++;
                    if (users >= 1) continue;                       // 快照里已经有 1 个农民 → 不再派人
                    if (m_farmTaken.count(fb.SN)) continue;         // 【修复·挤同一块田】本帧刚派过人 → 也不派
                    double d = calDistance(f.DR, f.UR,
                                           (double)fb.BlockDR * BLOCKSIDELENGTH,
                                           (double)fb.BlockUR * BLOCKSIDELENGTH);
                    if (d < bestFarmD) { bestFarmD = d; farmSN = fb.SN; }
                }
                if (farmSN >= 0) {
                    HumanAction(f.SN, farmSN);
                    m_issued.insert(f.SN);
                    m_farmTaken.insert(farmSN);     // 【修复·挤同一块田】记下"本帧这块田有人了" ✗ 别人别再派
                    m_orderTarget[f.SN] = farmSN; m_orderFrame[f.SN] = info.GameFrame;
                m_orderX[f.SN] = f.DR; m_orderY[f.SN] = f.UR;
                    m_foodGatherers.insert(f.SN);
                    m_role[f.SN] = 5;            // 工种：农田
                    foodCnt++;
                    continue;
                }
                // 【用户要求·修正】闲置的"食物系"农民（浆果/打猎/种田出身）没有猎物、也没有空田
                //   → 让他**自己去建一块农田**：他本来就闲着，不是从伐木/采金里抽调的人，
                //   不会影响其它工种；而唯一的专职建造者常被 马厩/学院/箭塔 占着，农田永远轮不到
                //   → 实测"只有一片田在采、其余猎人不动"。多人可并行建田，田很快补齐。
                if (isFood || prevRole == 1 || prevRole == 4 || prevRole == 5) {
                    int farmWantV = (info.GameFrame > FRAME_WAVE2)
                                    ? ((int)info.farmers.size() / 2) : 3;
                    if (farmWantV < FARM_MIN_COUNT) farmWantV = FARM_MIN_COUNT;
                    if (farmWantV > FARM_MAX_COUNT) farmWantV = FARM_MAX_COUNT;
                    if (countBuilding(info, BUILDING_FARM) < farmWantV
                        && (!mapFoodLeft(info) || info.Meat < 200)
                        && info.Wood >= ((info.Meat < 150) ? 80 : FARM_WOOD_GATE)) {   // ★食物告急→门槛放宽到 80，避免死锁
                        int gxf = m_centerX, gyf = m_centerY;
                        for (const tagBuilding& b : info.buildings)
                            if (b.Type == BUILDING_GRANARY) { gxf = b.BlockDR; gyf = b.BlockUR; break; }
                        int xf, yf;
                        if (findBuildBlock(info, xf, yf, 3, 3, gxf, gyf)) {
                            HumanBuild(f.SN, BUILDING_FARM, xf, yf);
                            m_issued.insert(f.SN);
                            continue;
                        }
                    }
                }
            }
        }
        // ⑤ 黄金：【发育策略】所有人都可以去采金（多余农民去采金，避免全堆到伐木）
        if (goldCnt < targetGold) {
            int sn = findNearestResource(info, RESOURCE_GOLD, f.SN);
            if (sn >= 0) {
                HumanAction(f.SN, sn);
                m_issued.insert(f.SN);
                m_orderTarget[f.SN] = sn; m_orderFrame[f.SN] = info.GameFrame;
                m_orderX[f.SN] = f.DR; m_orderY[f.SN] = f.UR;
                m_role[f.SN] = 3;                // 工种：采金
                goldCnt++;
                continue;
            }
        }
        // ⑦ 兜底：依次尝试，且都受配额限制（【修复】不再无限塞人砍树）
        int sn = findNearestHunt(info, f.SN);
        int newRole = 4;
        if (sn < 0 && goldCnt < targetGold) {
            sn = findNearestResource(info, RESOURCE_GOLD, f.SN);
            if (sn >= 0) { goldCnt++; newRole = 3; }
        }
        if (sn < 0 && woodCnt < targetWood) {
            sn = findNearestTree(info, f.SN);
            if (sn >= 0) { woodCnt++; newRole = 2; }
        }
        // 【用户要求·铜器后】兜底不再乱塞采金（黄金固定 5 人）：
        //   木材不够(<150)时临时加人伐木（最多到 6 个）；实在没活且黄金不足 5 人才去补位
        if (sn < 0 && bronze && info.Wood < 150 && woodCnt < 6) {
            sn = findNearestTree(info, f.SN);
            if (sn >= 0) { woodCnt++; newRole = 2; }
        }
        if (sn < 0 && bronze && goldCnt < targetGold) {
            sn = findNearestResource(info, RESOURCE_GOLD, f.SN);
            if (sn >= 0) { goldCnt++; newRole = 3; }
        }
        if (sn >= 0) {
            HumanAction(f.SN, sn);
            m_issued.insert(f.SN);
            m_orderTarget[f.SN] = sn; m_orderFrame[f.SN] = info.GameFrame;
                m_orderX[f.SN] = f.DR; m_orderY[f.SN] = f.UR;
            m_role[f.SN] = newRole;
        }
    }
}

// 找最近的可种农田（返回农田建筑 SN，找不到返回 -1）
int UsrAI::findNearestFarm(const tagInfo& info, int farmerSN)
{
    const tagFarmer* f = nullptr;
    for (const tagFarmer& ff : info.farmers)
        if (ff.SN == farmerSN) { f = &ff; break; }
    if (f == nullptr) return -1;

    int sn = -1;
    double best = 1e18;
    for (const tagBuilding& b : info.buildings) {
        if (b.Type != BUILDING_FARM || b.Percent < 100) continue;
        double d = calDistance(f->DR, f->UR,
                               (double)b.BlockDR * BLOCKSIDELENGTH, (double)b.BlockUR * BLOCKSIDELENGTH);
        if (d < best) { best = d; sn = b.SN; }
    }
    return sn;
}

// 打猎：分猎物类型差异化策略，防村民被大象打死
//   · 只统计"正在采/正在前往"该猎物的猎人（空闲农民 WorkObjectSN 有残留，不能算）
//   · 羚羊/狮子：安全猎物，两两一组（MAX_HUNTER_PER_PREY=2），优先打完
//   · 大象：危险（攻10，村民25血3下死），必须多人合作：
//       - 已有羚羊/狮子没打完 → 先不打大象（安全优先）
//       - 打大象：凑足 3 人以上才开打（1-2人去=送死）；最多 4 人集火
//   · 尸体按体型放宽：大象(300食物) 最多 3 人采，羚羊(150) 最多 2 人，狮子(100) 1 人
//   · 全部满员/没有可安全打的猎物 → 返回 -1（农民去砍树/种田，不站着发呆）
// ============================================================
// 【修复·农民被派去送死】资源安全判定
//   探路兵照亮敌营后，那些"离敌营很近 / 离大本营很远"的资源也会进 info.resources，
//   采集逻辑可能把农民派过去 → 路上/到达后被敌人打死 ✗
//   规则（任一不满足 → 不采）：
//     ① 离自家市中心 ≤ RES_SAFE_HOME_DIST 格（太远，路上就被截）
//     ② 离任何"已知敌方建筑" ≥ RES_SAFE_ENEMY_DIST 格（太近 = 送到敌人面前）
//   注：用平方距离自己算 —— 本助手是文件作用域 static，且调用点可能在 const 函数里，
//       基类的 calDistance 不是 const 成员，不能在那里用。
// ============================================================
// 【用户要求·安全的资源就去采】原来纯距离一刀切（>38 格就判不安全 ✗）
//   → 地图上安全的食物/猎物可能就采不到，农民全跑去伐木 ✗
//   放宽到 60 格，并新增"离可见敌方单位"检查（比只看敌方建筑更准 ✓）
static const int RES_SAFE_HOME_DIST  = 60;   // 采集半径（格），可调
static const int RES_SAFE_ENEMY_UNIT_DIST = 12;  // 离可见敌方单位多近算危险（格）
static const int RES_SAFE_ENEMY_DIST = 14;   // 离已知敌方建筑的安全距离（格），可调

static bool resSafe(const tagInfo& info, const tagResource& r)
{
    const int hx = (m_centerX >= 0) ? m_centerX : MAP_L / 2;
    const int hy = (m_centerY >= 0) ? m_centerY : MAP_U / 2;
    const double dx = (double)(r.BlockDR - hx);
    const double dy = (double)(r.BlockUR - hy);
    if (dx * dx + dy * dy > (double)RES_SAFE_HOME_DIST * (double)RES_SAFE_HOME_DIST)
        return false;                                    // 太远
    for (const tagBuilding& b : info.enemy_buildings) {
        const double ex = (double)(r.BlockDR - b.BlockDR);
        const double ey = (double)(r.BlockUR - b.BlockUR);
        if (ex * ex + ey * ey < (double)RES_SAFE_ENEMY_DIST * (double)RES_SAFE_ENEMY_DIST)
            return false;                                // 太靠近敌营
    }
    // 【用户要求·安全就去采】新增：离**可见敌方单位**太近也算危险 ✓
    //   （敌方单位只在当前视野内出现在快照里；看不见时这条不触发 ✓）
    for (const tagArmy& e : info.enemy_armies) {
        const double ux = (double)(r.BlockDR - (int)(e.DR / BLOCKSIDELENGTH));
        const double uy = (double)(r.BlockUR - (int)(e.UR / BLOCKSIDELENGTH));
        if (ux * ux + uy * uy < (double)RES_SAFE_ENEMY_UNIT_DIST * (double)RES_SAFE_ENEMY_UNIT_DIST)
            return false;                                // 敌人就在旁边
    }
    // ④ 【用户要求·阵亡点标记】离任何"未过期的危险点"（农民在远处被杀的现场）太近 → 不安全 ✗
    //    危险点 DANGER_LIFE(2 分钟) 后自动失效 ✓ → 敌人走了就能再去采 ✓
    for (int di = 0; di < m_dangerN; ++di) {
        if (info.GameFrame - m_dangerF[di] > DANGER_LIFE) continue;
        const double ax = (double)(r.BlockDR - m_dangerX[di]);
        const double ay = (double)(r.BlockUR - m_dangerY[di]);
        if (ax * ax + ay * ay < (double)DANGER_R * (double)DANGER_R)
            return false;                                // 这里刚死过农民 ✗
    }
    return true;
}

int UsrAI::findNearestHunt(const tagInfo& info, int farmerSN)
{
    (void)farmerSN;
    m_huntWaiting = false;   // 【发育策略】本次查询是否"想打猎但缺搭档"
    // 统计每个猎物的猎人数量（WORKING=正在采，WALKING=正前往）
    std::unordered_map<int,int> cnt;
    int idleFarmers = 0;   // 空闲农民数（判断能否凑足人打大象）
    for (const tagFarmer& f : info.farmers) {
        if (f.NowState == HUMAN_STATE_WORKING || f.NowState == HUMAN_STATE_WALKING)
            cnt[f.WorkObjectSN]++;
        else if (f.NowState == HUMAN_STATE_IDLE) idleFarmers++;
    }

    std::vector<int> corpses, alives;
    for (const tagResource& r : info.resources) {
        if (r.Type != RESOURCE_GAZELLE && r.Type != RESOURCE_ELEPHANT && r.Type != RESOURCE_LION) continue;
        if (r.Cnt <= 0) continue;
        if (!resSafe(info, r)) continue;                 // ★离敌营太近/离基地太远的猎物不碰
        if (r.Blood <= 0) corpses.push_back(r.SN);
        else alives.push_back(r.SN);
    }

    // ① 优先羚羊/狮子（安全猎物）：两两一组，猎人最少的优先
    //    （大象不在这一轮——羚羊没打完就不打大象）
    int safeBestSn = -1;
    int safeBestCnt = 1e9;
    for (int sn : alives) {
        const tagResource* rr = nullptr;
        for (const tagResource& r : info.resources)
            if (r.SN == sn) { rr = &r; break; }
        if (rr == nullptr) continue;
        if (rr->Type == RESOURCE_ELEPHANT) continue;      // 大象单独处理
        int c = cnt[sn];
        if (c >= MAX_HUNTER_PER_PREY) continue;           // 已满员 → 换下一只
        // 【发育策略】打猎要两人同去：缺搭档时标记"等待"，由调用方让该农民原地不动
        //   （等第二个农民生成后，两人同一帧一起被派去打同一只猎物）
        if (c == 0 && idleFarmers < 2) { m_huntWaiting = true; continue; }
        if (c < safeBestCnt) { safeBestCnt = c; safeBestSn = sn; }
    }
    if (safeBestSn >= 0) return safeBestSn;

    // ② 羚羊/狮子都满员或没有了 → 才考虑大象
    //    大象必须多人集火：至少 3 人（含已在打/前往的）才安全；不足 3 人不开打（防送死）
    //    已有人开打（c>=1）→ 补员到最多 4 人；没人打（c==0）且空闲农民 <3 → 也不开
    if (!alives.empty()) {
        int eleBestSn = -1;
        int eleBestCnt = 1e9;
        bool anyElephant = false;
        for (int sn : alives) {
            const tagResource* rr = nullptr;
            for (const tagResource& r : info.resources)
                if (r.SN == sn) { rr = &r; break; }
            if (rr == nullptr || rr->Type != RESOURCE_ELEPHANT) continue;
            anyElephant = true;
            int c = cnt[sn];
            // 补员规则：c>=4 已够；c==0 且空闲农民不足 3 → 不开新局（人等够了再说）
            if (c >= ELE_TEAM_MAX) continue;      // 上限 4 → 5（人多更安全 ✓）
            // 【用户要求·三人组队同时打】没人开打且空闲农民不足 3 个 →
            //   **让他原地等队友**（m_huntWaiting → 调用方本帧不给他派别的活 ✓）
            //   原来只是 continue ✗ → 他会掉到采尸/伐木分支去砍树 ✗ → 永远凑不齐人 ✗
            if (c == 0 && idleFarmers < ELE_TEAM_MIN) { m_huntWaiting = true; continue; }
            if (c < eleBestCnt) { eleBestCnt = c; eleBestSn = sn; }
        }
        if (anyElephant && eleBestSn >= 0) {
            // 【大象组队】本帧最多派 ELE_TEAM_MAX 个农民上这头大象 → 同一帧一起开打 ✓
            //   （下限由上面"idleFarmers < ELE_TEAM_MIN 就原地等"保证 ✓）
            if (m_eleSentSN != eleBestSn) { m_eleSentSN = eleBestSn; m_eleSentCount = 0; }
            if (m_eleSentCount >= ELE_TEAM_MAX) return -1;   // 本帧已够人 → 不再派
            m_eleSentCount++;
            return eleBestSn;
        }
    }

    // ③ 采尸：按体型放宽上限（大象多人采效率高；羚羊2人、狮子1人）
    if (!corpses.empty()) {
        int bestSn = -1;
        int bestCnt = 1e9;
        for (const tagResource& r : info.resources) {
            if (r.Blood > 0 || r.Cnt <= 0) continue;
            if (r.Type != RESOURCE_GAZELLE && r.Type != RESOURCE_ELEPHANT && r.Type != RESOURCE_LION) continue;
            int c = cnt[r.SN];
            int cap = (r.Type == RESOURCE_ELEPHANT) ? 3 : (r.Type == RESOURCE_GAZELLE ? 2 : 1);
            if (c >= cap) continue;
            if (c < bestCnt) { bestCnt = c; bestSn = r.SN; }
        }
        if (bestSn >= 0) return bestSn;
    }
    return -1;   // 没有可安全打的猎物 → 农民去砍树/种田，不硬塞
}

// 找最近指定类型的资源（返回资源 SN，找不到返回 -1）
int UsrAI::findNearestResource(const tagInfo& info, int type, int farmerSN)
{
    // 找到该农民的坐标
    const tagFarmer* f = nullptr;
    for (const tagFarmer& ff : info.farmers)
        if (ff.SN == farmerSN) { f = &ff; break; }
    if (f == nullptr) return -1;

    int sn = -1;
    double best = 1e18;
    for (const tagResource& r : info.resources) {
        if (r.Type != type || r.Cnt <= 0) continue;      // 只找对应类型且还有剩余的资源
        if (isBadTarget(r.SN, info.GameFrame)) continue; // 近期判定"卡住/不可达" → 换一个目标
        if (!resSafe(info, r)) continue;                 // ★离敌营太近/离基地太远的不采
        double d = calDistance(f->DR, f->UR, r.DR, r.UR);
        if (d < best) { best = d; sn = r.SN; }
    }
    return sn;
}

// 统计我方已建成的某类建筑数量
int UsrAI::countBuilding(const tagInfo& info, int type) const
{
    int cnt = 0;
    for (const tagBuilding& b : info.buildings)
        if (b.Type == type && b.Percent >= 100) cnt++;
    return cnt;
}

// 【用户要求】地图上还有浆果/动物（未被采完）→ 优先采它们，农田先不开/不扩
//   浆果丛：RESOURCE_BUSH；猎物：羚羊/大象/狮子（都按 Cnt>0 判断）
bool UsrAI::mapFoodLeft(const tagInfo& info) const
{
    // 【修复】只承认"离基地 40 格内"的浆果/猎物：地图另一头的一只动物，不该让我们一直不开农田
    double cx = (double)m_centerX * BLOCKSIDELENGTH;
    double cy = (double)m_centerY * BLOCKSIDELENGTH;
    for (const tagResource& r : info.resources) {
        if (r.Cnt <= 0) continue;
        if (r.Type != RESOURCE_BUSH && r.Type != RESOURCE_GAZELLE
            && r.Type != RESOURCE_ELEPHANT && r.Type != RESOURCE_LION) continue;
        // 【编译修复】本函数是 const，而基类的 calDistance 不是 const 成员 → 这里自己算平方距离
        double dx = r.DR - cx, dy = r.UR - cy;
        if (dx * dx + dy * dy > (40.0 * BLOCKSIDELENGTH) * (40.0 * BLOCKSIDELENGTH)) continue;   // 太远 → 视为没有
        return true;
    }
    return false;
}

// 统计我方某兵种数量
int UsrAI::countArmy(const tagInfo& info, int sort) const
{
    int cnt = 0;
    for (const tagArmy& a : info.armies)
        if (a.Sort == sort) cnt++;
    return cnt;
}

// 找砍树的树：分散选树，避免两个樵夫扎堆同一棵/相邻的树互相卡住
//   · 统计每棵树已被几个农民使用（WORKING/WALKING 指向它）
//   · 树与树距离很近时视为同一组（树是静态障碍，贴着采会卡采集位）
//   · 优先选"使用人数最少"的树；人数相同选更近的
int UsrAI::findNearestTree(const tagInfo& info, int farmerSN)
{
    const tagFarmer* f = nullptr;
    for (const tagFarmer& ff : info.farmers)
        if (ff.SN == farmerSN) { f = &ff; break; }
    if (f == nullptr) return -1;

    // 统计每棵树的使用人数
    std::unordered_map<int,int> cnt;
    for (const tagFarmer& w : info.farmers)
        if (w.NowState == HUMAN_STATE_WORKING || w.NowState == HUMAN_STATE_WALKING)
            cnt[w.WorkObjectSN]++;

    // 收集所有树（含剩余量的）
    // 【修复·2 树被围住挤不进去】只把"8 邻域里至少有一格能站人"的树算作候选：
    //   树在引擎里是静态障碍；一棵被其他树/石头/建筑围死的树，农民永远挤不到它旁边
    //   （引擎要求贴到半格内才能采）→ 他站在树丛外圈罚站、砍不到木头 →
    //   120 帧后被判卡、拉黑、拽回市中心 → 再换一棵更内圈的树……
    //   实测：木头停在 10，面板里"设置工作目标为 树 X"紧跟"移动至(市中心)"反复刷。
    //   只挑"能从外圈站进去"的树后，砍伐顺序自然变成由外向内，
    //   外圈砍掉后内圈的树自动变成可站 → 不会再出现挤不进去的目标。
    //   兜底：万一所有树都挤不进去（极端地图），退回原行为，不至于完全没柴可砍。
    std::vector<const tagResource*> trees;
    std::vector<const tagResource*> treesAll;
    {
        // 复用现成的 isStaticBlock：海洋/建筑/静态资源 = 不可站，空地/单位 = 可站
        auto standable = [&](int nx, int ny) -> bool {
            if (nx < 0 || nx >= 100 || ny < 0 || ny >= 100) return false;
            return !isStaticBlock(nx, ny);
        };
        auto treeOk = [&](const tagResource* t) -> bool {
            int bx = (int)(t->DR / BLOCKSIDELENGTH);
            int by = (int)(t->UR / BLOCKSIDELENGTH);
            for (int dx = -1; dx <= 1; ++dx)
                for (int dy = -1; dy <= 1; ++dy) {
                    if (dx == 0 && dy == 0) continue;
                    if (standable(bx + dx, by + dy)) return true;   // 邻域有一格能站 → 砍得到
                }
            return false;
        };
        for (const tagResource& r : info.resources) {
            if (r.Type != RESOURCE_TREE || r.Cnt <= 0) continue;
            if (!resSafe(info, r)) continue;                 // ★离敌营太近/离基地太远的树不砍
            treesAll.push_back(&r);
            if (treeOk(&r)) trees.push_back(&r);
        }
        if (trees.empty()) trees = treesAll;      // 全挤不进去 → 退回原行为
    }
    if (trees.empty()) return -1;

    int bestSn = -1;
    double bestScore = 1e18;
    // 【发育策略】伐木优先选"离仓库/市政中心近"的树——木头存放在市中心/仓库，缩短往返
    auto depotDist = [&](const tagResource* t) {
        double best = 1e18;
        for (const tagBuilding& b : info.buildings) {
            if (b.Type != BUILDING_CENTER && b.Type != BUILDING_STOCK) continue;
            double dd = calDistance(t->DR, t->UR,
                                    (double)b.BlockDR * BLOCKSIDELENGTH,
                                    (double)b.BlockUR * BLOCKSIDELENGTH);
            if (dd < best) best = dd;
        }
        return best;
    };
    // 【用户反馈·"一个人砍树、另一个人在身后转"】
    //   原注释写"树挨着树视为同一组"，但实现只统计了"同一棵树"的使用人数 → 两个樵夫被派到
    //   两棵紧挨着的树；后到者的采集位（相邻格）被前者占着，寻路反复失败，就在别人身后转圈。
    //   这里补上"邻域使用数"：相邻 1.5 格内的树算作同一个"采集位争夺区"。
    std::unordered_map<int,int> treeAt;            // 块坐标(x*100+y) -> 树SN（快速找相邻树）
    for (const tagResource* t : trees) {
        int bx = (int)(t->DR / BLOCKSIDELENGTH);
        int by = (int)(t->UR / BLOCKSIDELENGTH);
        treeAt[bx * 100 + by] = t->SN;
    }
    std::unordered_map<int,int> nearCnt;           // 树SN -> 邻域（不含自己）使用人数
    for (const tagResource* t : trees) {
        int u = 0;
        int bx = (int)(t->DR / BLOCKSIDELENGTH);
        int by = (int)(t->UR / BLOCKSIDELENGTH);
        for (int dx = -2; dx <= 2; ++dx) {
            for (int dy = -2; dy <= 2; ++dy) {
                if (dx == 0 && dy == 0) continue;
                auto it = treeAt.find((bx + dx) * 100 + (by + dy));
                if (it == treeAt.end()) continue;
                double dd = calDistance(t->DR, t->UR,
                                        (double)(bx + dx) * BLOCKSIDELENGTH,
                                        (double)(by + dy) * BLOCKSIDELENGTH);
                if (dd > 1.5 * BLOCKSIDELENGTH) continue;    // 只算真正紧挨的
                u += cnt[it->second];
            }
        }
        nearCnt[t->SN] = u;
    }
    auto scoreOf = [&](const tagResource* t, double extra) {
        double d = calDistance(f->DR, f->UR, t->DR, t->UR);
        return extra + depotDist(t) * 0.6 + d * 0.4;
    };

    // 第一轮：整组（自己 + 紧邻树）都没人用 → 最理想，彻底不会抢采集位
    for (const tagResource* t : trees) {
        if (isBadTarget(t->SN, info.GameFrame)) continue;
        if (cnt[t->SN] > 0 || nearCnt[t->SN] > 0) continue;
        double score = scoreOf(t, 0.0);
        if (score < bestScore) { bestScore = score; bestSn = t->SN; }
    }
    if (bestSn >= 0) return bestSn;

    // 第二轮：自己这棵没人用（紧邻树有人）→ 次优，仍然不会和别人抢同一棵树
    for (const tagResource* t : trees) {
        if (isBadTarget(t->SN, info.GameFrame)) continue;
        if (cnt[t->SN] > 0) continue;
        double score = scoreOf(t, (double)nearCnt[t->SN] * 6.0 * BLOCKSIDELENGTH);
        if (score < bestScore) { bestScore = score; bestSn = t->SN; }
    }
    if (bestSn >= 0) return bestSn;

    // 第三轮（兜底·树少人多）：选"邻域最空 + 最近"的，并尽量避开已经有 2 人的树
    bestScore = 1e18;
    for (const tagResource* t : trees) {
        if (isBadTarget(t->SN, info.GameFrame)) continue;
        double extra = (double)nearCnt[t->SN] * 6.0 * BLOCKSIDELENGTH;
        if (cnt[t->SN] >= 2) extra += 30.0 * BLOCKSIDELENGTH;
        double score = scoreOf(t, extra);
        if (score < bestScore) { bestScore = score; bestSn = t->SN; }
    }
    return bestSn;
}

// ============================================================
// 市镇中心：升级铜器（优先）→ 分阶段生产农民
// 农民节奏：第一波前 12（开局8+新4采浆果）→ 第二波前 16（再新4打猎）→ 之后 20
// 升级条件：市场/靶场/马厩 已建 2 个 + 800 食物（升级优先，保证按时升铜器）
// ============================================================
void UsrAI::manageCenter(const tagInfo& info)
{
    // 【发育策略】农民目标：
    //   · 前期：造到 20 人口（4 座房 = 20 上限）为止；靶场后补的 2 座房留给兵力，不超产农民
    //   · 铜器后且"金矿旁仓库"已建（仓库数≥2）→ 补到 24，新农民去采金
    bool bronzeNow = (info.civilizationStage >= CIVILIZATION_BRONZEAGE);
    // 【3.0.7g 修复】补农民到 24 前先看有没有人口留给军队：
    //   人口上限 - 现有农民 < 12（要留给军队的人口）→ 不扩农，避免又把自己卡成 0 兵
    bool needGoldFarmers = (bronzeNow && countBuilding(info, BUILDING_STOCK) >= 2
                            && (int)info.Human_MaxNum - (int)info.farmers.size() >= 12);
    int farmerTarget = 20;
    if (needGoldFarmers) farmerTarget = 24;

    for (const tagBuilding& b : info.buildings) {
        if (b.Type != BUILDING_CENTER) continue;
        if (b.Percent < 100 || b.Project != ACT_NULL) continue;   // 建造中或正在生产
        if (m_issued.count(b.SN)) continue;                        // 本帧已下令

        // 1) 升级铜器（最高优先：保证第二波前升完，避免第二波损失农民后补人口吃掉升级食物）
        if (info.civilizationStage < CIVILIZATION_BRONZEAGE
            && canUpgradeBronze(info)
            && info.Meat >= BUILDING_CENTER_UPGRADE_BRONZEAGE_FOOD) {
            BuildingAction(b.SN, BUILDING_CENTER_UPGRADE);
            m_issued.insert(b.SN);
            m_bronzeUpgradeFrame = info.GameFrame;   // 记录升级开始帧（采金人数按它递增）
            return;     // 本帧中心只做一件事
        }
        // 2) 生产农民：升级建筑没建齐时正常补农民；建齐后停补、全力攒 800 食物升级
        //    铜器后：默认不再造农民（人口留给兵力）；仅当"金矿旁仓库"建好才补人到 24 去采金
        bool upgradeBuildingReady = (!bronzeNow && canUpgradeBronze(info));
        if (!upgradeBuildingReady
            && (!bronzeNow || needGoldFarmers)
            && (int)info.farmers.size() < farmerTarget
            && info.Human_Num < info.Human_MaxNum
            && info.Meat >= BUILDING_CENTER_CREATEFARMER_FOOD) {
            BuildingAction(b.SN, BUILDING_CENTER_CREATEFARMER);
            m_issued.insert(b.SN);
            return;     // 本帧中心只做一件事
        }
    }
}

// 是否满足升级铜器条件：市场/马厩/靶场 中已建成 2 个
bool UsrAI::canUpgradeBronze(const tagInfo& info) const
{
    int cnt = 0;
    if (countBuilding(info, BUILDING_MARKET) > 0) cnt++;
    if (countBuilding(info, BUILDING_STABLE) > 0) cnt++;
    if (countBuilding(info, BUILDING_RANGE) > 0) cnt++;
    return cnt >= 2;
}

// ============================================================
// 建筑规划（用户指定顺序）：
//   住房5座 → 箭塔1座 → 兵营 → 市场（升级必需！）→ 靶场（升级必需）
//   → 升级后首选：马厩 → 学院 → 农田 → 羚羊堆旁仓库
// 【用户要求·3.0.7g 修正】**所有基地建筑只由专职建造者（m_builderSN）一个人建**
//   实测问题：并行建造（抽调第二个空闲农民 + 靶场紧急抽调伐木工）导致
//     ① 开局有 2 个人在建房屋（伐木只剩 2 人）
//     ② 新生成的农民刚出生（IDLE）就被抓去建市场 → 不去采果/伐木
//   现在：建造者正在忙 → 本帧跳过，等他建完当前建筑再接下一条（串行建造）
// ============================================================
void UsrAI::buildBuildings(const tagInfo& info)
{
    // 找专职建造者：只有他"空闲且本帧没被下令"时才派活
    int builder = -1;
    if (m_builderSN >= 0) {
        for (const tagFarmer& f : info.farmers) {
            if (f.SN != m_builderSN) continue;
            if (f.NowState == HUMAN_STATE_IDLE && !m_issued.count(f.SN)) builder = f.SN;
            break;
        }
    }
    if (builder == -1) return;      // 没有建造者/他正在建 → 绝不占用采集农民

    {

        bool built = false;
        // 【发育策略·建造线】住房×2（共4座=20人口）→ 箭塔1座 → 市场（随即升伐木科技）
        //   → 兵营 → 靶场（建在箭塔附近）→ 靶场建成后补2座房（共6座=28人口，给兵力腾人口）
        //   → 铜器后：金矿旁仓库 → 马厩/学院/农田
        // 房屋目标：靶场未建=4座(20人口)；靶场建成=6座(28人口)；铜器=8座(36人口)
        // 【3.0.7g 关键修复·第二波没兵】config.json HOUSE_HUMAN_NUM=4 且市中心也算1座：
        //   6 座房只有 28 人口，而"20 农民 + 祭司 + 第一波祭司转化的敌方单位"正好占满
        //   → trainArmy 首行 Human_Num >= Human_MaxNum 判断直接 return → 第二波一个新兵都造不出来，
        //     只能靠第一波转化的部队硬顶（实测现象）。
        //   现在：住房阶梯 4/6/8 不变；**8 座达成后一口气补到 12 座**（≈50 人口，到上限）。
        //   注意只在靶场已建后才补，避免抢在"市场/兵营/靶场"这条升级关键链之前。
        int homes = countBuilding(info, BUILDING_HOME);
        bool bronzeNow2 = (info.civilizationStage >= CIVILIZATION_BRONZEAGE);
        // 【ai_log 实证·头号死锁】原来造房目标被绑在"靶场已建"上 ✗：
        //   靶场要 市场+兵营+靶场 = 425 木 ✗，木头不够 → 靶场永远建不出
        //   → 人口永远 20 → trainArmy 首行直接 return → **兵永远 0** ✗✗（f=6011: pop20/20 army0）
        //   现在与靶场**解耦**，改成按人口需求（文档要求 50 人口 ≈ 12 座房 ✓）：
        int houseTarget = bronzeNow2 ? 12 : 6;                 // 非铜器 6 座(24 人口)，铜器后 12 座(48 人口)
        if ((int)info.Human_Num >= (int)info.Human_MaxNum - 2) houseTarget += 2;   // 人口告急 → 紧急补房 ✓
        if (houseTarget > 12) houseTarget = 12;
        // 【用户要求】原来的 4/6/8 阶梯逻辑**不动**；8 座达成后**不等人口告急**，
        //   直接把目标抬到 12 → 一口气从 8 座建到 12 座（≈50 人口，正好到上限）。
        //   原来这里是"人口≥上限-2 就把目标设为 现有房数+1"，每次只加 1 座、还得等人口再满
        //   —— 现在按"房≥8"直接触发，不再依赖人口。
        // 【马厩前提】保持低门槛（4/6 座），不要因为"要建 12 座"把马厩一直拖住 ✗
        int houseNormalTarget = bronzeNow2 ? 6 : 4;
        // 【农田不再由专职建造者负责】原来这里算 farmWant（第二波后按食物采集人数扩田）。
        //   现在农田全部由食物系农民自己建，本函数里的两处农田分支都已删除 → 不再需要它。

        // 记录箭塔位置（靶场要建在箭塔附近）
        int towerBX = -1, towerBY = -1;
        for (const tagBuilding& tb : info.buildings)
            if (tb.Type == BUILDING_ARROWTOWER) { towerBX = tb.BlockDR; towerBY = tb.BlockUR; break; }

        // 【已删除·用户要求】原"紧急·食物告急 → 专职建造者补农田"。
        //   农田不再由专职建造者建造：改由"要采这块田的食物系农民"自己建
        //   （manageVillagers 里"空闲的食物系农民 → 自己去建一块农田"，可多人并行）。
        //   原来这一条条件(食<150)长期成立，会把建造者永久霸占 → 马厩永远轮不到。

        // 【已删除·用户要求】原"紧急·人口满 → 补房"入口。
        //   住房现在只在"优先级 2"这一处建（目标 4/6/8，8 座后一口气到 12），不再有第二个入口。

        // ===== 【用户要求·建造优先级重排】1 马厩 → 2 住房 → 3 金矿旁仓库 → 4 学院 =====
        //   马厩：前提是"兵营在 + 靶场已建 + 住房正常目标(8/6)已完成"，也就是
        //   "靶场和 8 座住房什么的都建完之后"第一个建它；放在最前面是为了不被
        //   后面的补房/补仓库饿死（原来它排在"紧急补房"之后 → 永远轮不到）。
        if (!built && info.civilizationStage >= CIVILIZATION_BRONZEAGE   // 前提：铜器后
            && countBuilding(info, BUILDING_ARMYCAMP) > 0     // 引擎前置：马厩←兵营
            && countBuilding(info, BUILDING_RANGE) > 0        // 靶场已建
            && homes >= houseNormalTarget                     // 住房目标(8/6)已完成
            && countBuilding(info, BUILDING_STABLE) == 0 && info.Wood >= BUILD_STABLE_WOOD) {
            int x, y;
            if (findBuildBlock(info, x, y, 3, 3)) {
                HumanBuild(builder, BUILDING_STABLE, x, y);
                m_issued.insert(builder);
                return;                     // 本帧只下这一条令（搬到高位后必须立刻结束）
            }
        }

        // ===== 2) 住房：目标 4→6→8（正常），8 座达成后一口气补到 12（不等人口告急）=====
        if (countBuilding(info, BUILDING_HOME) < houseTarget && info.Wood >= BUILD_HOUSE_WOOD) {
            int x, y;
            if (findBuildBlock(info, x, y, 2, 2)) {
                HumanBuild(builder, BUILDING_HOME, x, y);
                m_issued.insert(builder);
                return;                     // 本帧只下这一条令（搬到高位后必须立刻结束）
            }
        }

        // ===== 3) 金矿旁仓库（从原 15 号位提前到这里）=====
        // 【发育策略】金矿旁仓库：靶场已建、两座新房已补（房≥6）、铜器后 → 建设者去金矿旁建仓库
        //    建好后新生成的农民就近采金（原有伐木/种田农民不动）
        if (!built
            && countBuilding(info, BUILDING_RANGE) > 0
            && countBuilding(info, BUILDING_HOME) >= 6
            && countBuilding(info, BUILDING_STOCK) < 3
            && info.civilizationStage >= CIVILIZATION_BRONZEAGE
            && info.Wood >= BUILD_STOCK_WOOD) {
            const tagResource* gold = nullptr;
            double gbest = 1e18;
            double cx = (double)m_centerX * BLOCKSIDELENGTH;
            double cy = (double)m_centerY * BLOCKSIDELENGTH;
            for (const tagResource& r : info.resources) {
                if (r.Type != RESOURCE_GOLD || r.Cnt <= 0) continue;
                double d = calDistance(cx, cy, r.DR, r.UR);
                if (d < gbest) { gbest = d; gold = &r; }
            }
            if (gold != nullptr) {
                // 已有仓库离金矿是否够近（≤8格）；不够近才新建
                double nearestStock = 1e18;
                for (const tagBuilding& sb : info.buildings) {
                    if (sb.Type != BUILDING_STOCK) continue;
                    double d = calDistance(gold->DR, gold->UR,
                                           (double)sb.BlockDR * BLOCKSIDELENGTH,
                                           (double)sb.BlockUR * BLOCKSIDELENGTH);
                    if (d < nearestStock) nearestStock = d;
                }
                if (nearestStock > 8.0 * BLOCKSIDELENGTH) {
                    int x, y;
                    int gbx = (int)(gold->DR / BLOCKSIDELENGTH);
                    int gby = (int)(gold->UR / BLOCKSIDELENGTH);
                    if (findBuildBlock(info, x, y, 3, 3, gbx, gby)) {
                        HumanBuild(builder, BUILDING_STOCK, x, y);
                        m_issued.insert(builder);
                        return;                     // 本帧只下这一条令（搬到高位后必须立刻结束）
                    }
                }
            }
        }

        // ===== 4) 学院（从原 13 号位提前到这里；加"马厩已建"的引擎前置）=====
        if (!built && info.civilizationStage >= CIVILIZATION_BRONZEAGE
            && countBuilding(info, BUILDING_STABLE) > 0          // 引擎前置：学院←马厩
            && countBuilding(info, BUILDING_COLLAGE) == 0 && info.Wood >= BUILD_COLLAGE_WOOD) {
            int x, y;
            if (findBuildBlock(info, x, y, 3, 3)) {
                HumanBuild(builder, BUILDING_COLLAGE, x, y);
                m_issued.insert(builder);
                return;                     // 本帧只下这一条令（搬到高位后必须立刻结束）
            }
        }


        // ===== 【用户要求】靶场建好后立刻补箭塔（删掉链里的箭塔①后，第一座也由这里出）=====
        //   引擎强制前置（Development.cpp:768-769）：建塔需要"箭塔科技"，而该科技只能在谷仓研发
        //   （50 食 + 10 秒）。石头方面：BUILD_ARROWTOWER_STONE=150 = 开局 INITIAL_STONE=150，
        //   且我们全程不采石（targetStone=0）→ 这 150 石一直闲置，正好够第二座塔。
        //   优先块放在住房之前 → 不被"人口临界补房"挡住（这也是马厩/学院曾经被挡死的原因）。
        //   顺序：谷仓(120木) → 箭塔科技(50食) → 箭塔(150石)。
        if (!built && countBuilding(info, BUILDING_RANGE) > 0
            && countBuilding(info, BUILDING_ARROWTOWER) < 2
            && info.Stone >= BUILD_ARROWTOWER_STONE) {
            if (m_researchCount[BUILDING_GRANARY_ARROWTOWER] > 0) {
                // 科技已好 → 在第一座箭塔旁边建第二座（交叉火力）
                int tx2, ty2;
                bool found2 = false;
                if (towerBX >= 0) found2 = findBuildBlock(info, tx2, ty2, 2, 2, towerBX, towerBY);
                if (!found2) found2 = findBuildBlock(info, tx2, ty2, 2, 2);
                if (found2) {
                    HumanBuild(builder, BUILDING_ARROWTOWER, tx2, ty2);
                    m_issued.insert(builder);
                    return;                     // 本帧只下这一条建造令
                }
            } else if (countBuilding(info, BUILDING_GRANARY) == 0
                       && info.Wood >= BUILD_GRANARY_WOOD) {
                // 还没有谷仓（箭塔科技没地方研发）→ 先补一座谷仓
                int gx2, gy2;
                if (findBuildBlock(info, gx2, gy2, 3, 3)) {
                    HumanBuild(builder, BUILDING_GRANARY, gx2, gy2);
                    m_issued.insert(builder);
                    return;
                }
            }
        }

        // 【已上移·用户要求】原链里的"住房"已提到优先级 2（目标 4/6/8，8 座达成后到 12）。
        // 【已删除·用户要求】原链里的"箭塔①"。第一座箭塔改由"优先级 5"（箭塔②）负责，
        //   它要求靶场已建 → 正好是"靶场建好后立刻补箭塔"的顺序。
        // ===== 主链（互斥，只走一条）：市场 → 兵营 → 靶场 =====
        //   （原链首的"住房""箭塔①"已分别上移/删除，"马厩/学院/农田"也已移走）
        if (countBuilding(info, BUILDING_MARKET) == 0 && info.Wood >= BUILD_MARKET_WOOD) {
            int x, y;
            if (findBuildBlock(info, x, y, 3, 3)) {
                HumanBuild(builder, BUILDING_MARKET, x, y);
                m_issued.insert(builder);
                built = true;
            }
        }
        // 4) 兵营（靶场前置）
        else if (countBuilding(info, BUILDING_MARKET) > 0
            && countBuilding(info, BUILDING_ARMYCAMP) == 0 && info.Wood >= BUILD_ARMYCAMP_WOOD) {
            int x, y;
            if (findBuildBlock(info, x, y, 3, 3)) {
                HumanBuild(builder, BUILDING_ARMYCAMP, x, y);
                m_issued.insert(builder);
                built = true;
            }
        }
        // 5) 靶场（升级必需；【发育策略】建在箭塔附近）
        else if (countBuilding(info, BUILDING_MARKET) > 0
            && countBuilding(info, BUILDING_ARMYCAMP) > 0
            && countBuilding(info, BUILDING_RANGE) == 0 && info.Wood >= BUILD_RANGE_WOOD) {
            int x, y;
            bool found = false;
            if (towerBX >= 0) found = findBuildBlock(info, x, y, 3, 3, towerBX, towerBY);
            if (!found) found = findBuildBlock(info, x, y, 3, 3);
            if (found) {
                HumanBuild(builder, BUILDING_RANGE, x, y);
                m_issued.insert(builder);
                built = true;
            }
        }
        // 【已上移·用户要求】原链里的"马厩"已提到优先级 1。
        // 【已上移·用户要求】原链里的"学院"已提到优先级 4。
        // 【已删除·用户要求】原链里的"农田"。农田一律由食物系农民自己建。
        // 【已上移·用户要求】原 15 号位的"金矿旁仓库"已提到优先级 3。
        // （羚羊堆仓库/浆果堆谷仓由采集者负责，见 buildResourceDepots）
        // ===== 【策略文档·超级快速】靶场只建 1 个 → 出兵太慢 ✗ =====
        //   文档(71行)："理想情况下 8 分多一点就可以两个靶场同时出兵，10 分钟前 3 个靶场同时出兵"
        //   3 个靶场 = 每分钟约 6 个复合弓 ✓ —— 这是"边打边造、越打越强"的前提 ✓
        //   放在主链**之后**：优先级低于"升级必需链"，但木头够就补 ✓（留 60 木给别的用途 ✓）
        // round2#8 【修复·靶场建不出】科技不再是硬前置：第二波(13500)之后即便科技还没发起，
        //   也允许建第 2/3 座靶场（文档 71 行："8 分多一点两个靶场同时出兵"）
        if (!built && (m_researchCount[BUILDING_RANGE_UPGRADE_COMPOSITE_BOW] > 0
                       || info.GameFrame >= FRAME_WAVE2)
            && countBuilding(info, BUILDING_RANGE) > 0
            && countBuilding(info, BUILDING_RANGE) < 3
            && info.Wood >= BUILD_RANGE_WOOD + 60) {
            int x, y;
            bool found = false;
            if (towerBX >= 0) found = findBuildBlock(info, x, y, 3, 3, towerBX, towerBY);
            if (!found) found = findBuildBlock(info, x, y, 3, 3);
            if (found) {
                HumanBuild(builder, BUILDING_RANGE, x, y);
                m_issued.insert(builder);
                built = true;
            }
        }
        if (!built) return;  // 无可建建筑 → 本帧结束（绝不抽调其他农民帮忙）
    }
}

// ============================================================
// 【修复·谷仓重复建】统计某类建筑数量（**含在建的地基**）
//   原来的 countBuilding() 只数 Percent>=100（已建成），于是 buildResourceDepots 里
//   "刚下过令的谷仓"既不算"最近储存点"、也不计入数量上限：
//     · 距离判定 → 那堆浆果永远显得离储存点 > 8 格
//     · 数量上限 → < 3 永远成立
//   结果：只要建造者中途被打断一次（被打死 / 被 manageVillagers 抢去采集 /
//         找不到空地而被释放），下一帧就会在同一堆浆果旁**再下一个地基**，
//         而木头是下单即刻全额扣除的（Core_List.cpp:391）→ 白吃 120 木/座。
//   本函数是文件作用域的 static 自由函数 —— **不进类**，保证 sizeof(UsrAI) 不变。
// ============================================================
static int countBuildingAny(const tagInfo& info, int type)
{
    int cnt = 0;
    for (const tagBuilding& b : info.buildings)
        if (b.Type == type) cnt++;
    return cnt;
}

// ============================================================
// 资源点仓库/谷仓：固定 1 个"资源点建造者"负责（m_depotBuilderSN）
//   · 不再依赖"随机空闲农民"（经济满员时没人空闲 → 远处仓库永远建不出来）
//   · 建完仓库/谷仓后 → 就地采集最近的浆果/猎物（正好投入采集，食物就近存放）
//   · 建造中由本函数跨帧跟踪，不被其他模块重新分配
// ============================================================
void UsrAI::buildResourceDepots(const tagInfo& info)
{
    // ---- 判断是否需要建仓（【新策略】按"离最近储存点的距离"判断）----
    //   开局通常已有固定的浆果/猎物群；若探路后发现新的浆果/羚羊离现有储存点太远
    //   → 就近再建一个（浆果→谷仓，打猎肉→仓库），缩短往返
    //   阈值 8 格；每类最多 3 座（避免乱建浪费木头）
    const double NEED_DIST = 8.0 * BLOCKSIDELENGTH;

    // ① 找"离最近储存点最远的浆果丛"——它就是最需要就近储存的那一堆
    //    【修正】市镇中心可存放所有资源 → 距离判断要把市中心也算进去（近的话不用建）
    const tagResource* farBush = nullptr;
    double farBushD = 0;
    for (const tagResource& r : info.resources) {
        if (r.Type != RESOURCE_BUSH || r.Cnt <= 0) continue;
        double nearest = 1e18;
        for (const tagBuilding& b : info.buildings) {
            // 【修复·谷仓重复建】在建的谷仓/市中心也算"储存点"：
            //   原来这里有 if (b.Percent < 100) continue; 把地基排除掉了 →
            //   刚下过令的谷仓不算数 → 同一堆浆果的距离判定恒 > 8 格 → 反复下地基。
            //   （地基马上就要变成储存点，提前算上是正确的）
            if (b.Type != BUILDING_GRANARY && b.Type != BUILDING_CENTER) continue;
            double d = calDistance(r.DR, r.UR,
                                   (double)b.BlockDR * BLOCKSIDELENGTH,
                                   (double)b.BlockUR * BLOCKSIDELENGTH);
            if (d < nearest) nearest = d;
        }
        if (nearest > farBushD) { farBushD = nearest; farBush = &r; }
    }
    // ② 找"离最近储存点最远的猎物"（仓库 或 市中心）
    const tagResource* farPrey = nullptr;
    double farPreyD = 0;
    for (const tagResource& r : info.resources) {
        if (r.Type != RESOURCE_GAZELLE && r.Type != RESOURCE_ELEPHANT && r.Type != RESOURCE_LION) continue;
        if (r.Cnt <= 0) continue;
        double nearest = 1e18;
        for (const tagBuilding& b : info.buildings) {
            // 【修复·仓库重复建】在建的仓库/市中心也算"储存点"（同浆果堆那条）
            if (b.Type != BUILDING_STOCK && b.Type != BUILDING_CENTER) continue;
            double d = calDistance(r.DR, r.UR,
                                   (double)b.BlockDR * BLOCKSIDELENGTH,
                                   (double)b.BlockUR * BLOCKSIDELENGTH);
            if (d < nearest) nearest = d;
        }
        if (nearest > farPreyD) { farPreyD = nearest; farPrey = &r; }
    }
    // 【用户要求】储存点只做**距离判断**，不判断靶场（不等靶场建成，该建就建）
    //   谷仓（浆果）：离最近储存点（谷仓/市中心）> 8 格 → 就近建一个（最多 3 座）
    //   仓库（打猎肉）：【用户要求】只建一次！铜器前不再建第二个（"猎物建造仓库只进行一次"）
    //     下令后用"有没有真的出现仓库（含在建）"确认是否生效；
    //     若 600 帧后仍一个仓库都没有（建造者中途死亡等）→ 允许重下一次，避免永远没有打猎仓库
    bool needStock = (!m_preyStockDone && farPrey != nullptr && farPreyD > NEED_DIST
                      && countBuildingAny(info, BUILDING_STOCK) < 3
                      && info.Wood >= BUILD_STOCK_WOOD);
    if (m_preyStockDone) {
        bool anyStock = false;
        for (const tagBuilding& b : info.buildings)
            if (b.Type == BUILDING_STOCK) { anyStock = true; break; }
        if (!anyStock && info.GameFrame - m_preyStockFrame > 600) m_preyStockDone = false;
    }
    bool needGranary = (farBush != nullptr && farBushD > NEED_DIST
                        && countBuildingAny(info, BUILDING_GRANARY) < 3
                        && info.Wood >= BUILD_GRANARY_WOOD);
    // 没有任何要建的 + 没在役建造者 → 直接结束
    if (!needStock && !needGranary && m_depotBuilderSN < 0) return;

    // ---- 建造者状态机 ----
    const tagFarmer* builder = nullptr;
    if (m_depotBuilderSN >= 0) {
        for (const tagFarmer& f : info.farmers)
            if (f.SN == m_depotBuilderSN) { builder = &f; break; }
        if (builder == nullptr) m_depotBuilderSN = -1;   // 建造者死亡/消失 → 重新挑
    }
    if (m_depotBuilderSN < 0) {
        // 需要建仓时才挑人（不需要建则不占农民）
        if (!needStock && !needGranary) return;
        for (const tagFarmer& f : info.farmers) {
            if (f.FarmerSort != FARMERTYPE_FARMER) continue;
            if (f.NowState != HUMAN_STATE_IDLE) continue;
            if (m_issued.count(f.SN)) continue;
            if (f.SN == m_builderSN) continue;              // 不占用基地专职建造者
            m_depotBuilderSN = f.SN;
            builder = &f;
            break;
        }
        if (builder == nullptr) return;                     // 实在没有空闲农民 → 下帧再试
    }

    // 建造者正在建造/行走（非空闲）→ 不打扰，等建完
    if (builder->NowState != HUMAN_STATE_IDLE) return;

    // ---- 空闲状态：优先派他去建仓库/谷仓（就建在"最远的那一堆"资源旁）----
    if (needStock && farPrey != nullptr) {
        int gx = (int)(farPrey->DR / BLOCKSIDELENGTH);
        int gy = (int)(farPrey->UR / BLOCKSIDELENGTH);
        // 【修正】必须建在猎物堆 8 格内；附近实在没空地 → 这帧不建（绝不建到很远的地方）
        int x, y;
        if (findBuildBlockNear(info, x, y, 3, 3, gx, gy, 8)) {
            HumanBuild(m_depotBuilderSN, BUILDING_STOCK, x, y);
            m_issued.insert(m_depotBuilderSN);
            m_preyStockDone = true;              // 【用户要求】猎物仓库只建一次
            m_preyStockFrame = info.GameFrame;
            return;
        }
    }
    if (needGranary && farBush != nullptr) {
        int gx = (int)(farBush->DR / BLOCKSIDELENGTH);
        int gy = (int)(farBush->UR / BLOCKSIDELENGTH);
        // 【修正】必须建在浆果丛 8 格内；附近实在没空地 → 这帧不建
        int x, y;
        if (findBuildBlockNear(info, x, y, 3, 3, gx, gy, 8)) {
            HumanBuild(m_depotBuilderSN, BUILDING_GRANARY, x, y);
            m_issued.insert(m_depotBuilderSN);
            return;
        }
    }

    // ---- 没有要建的了 → 建完收尾：就地采集最近的浆果/猎物，然后释放回普通农民 ----
    int sn = findNearestResource(info, RESOURCE_BUSH, m_depotBuilderSN);
    if (sn < 0) sn = findNearestHunt(info, m_depotBuilderSN);
    if (sn >= 0) {
        HumanAction(m_depotBuilderSN, sn);
        m_issued.insert(m_depotBuilderSN);
    }
    m_depotBuilderSN = -1;   // 释放：下帧由 manageVillagers 正常管理（已下采集令，本帧不重分配）
}

// ============================================================
// 科技研发（完整科技链，铜器后按 PPT 优先级）
// 谷仓：解锁箭塔 → 铜器箭塔升级
// 市场：伐木 → 车轮(铜器) → 采金
// 仓库：工具使用 → 金属加工(铜器) → 步兵/弓兵/骑兵护甲 → 青铜盾
// 兵营：阔剑科技(铜器)    靶场：复合弓科技(铜器)
// ============================================================
void UsrAI::researchTech(const tagInfo& info)
{
    bool bronze = (info.civilizationStage >= CIVILIZATION_BRONZEAGE);
    // 攒升级食物期间（市场/靶场已齐、未升级、食物<800）：
    //   只允许谷仓研发箭塔科技（建塔防守必需），其余科技全部暂停——
    //   防止科技研发花掉食物，导致 800 升级食物永远攒不够（第二波前必须升完）
    bool savingForUpgrade = !bronze && canUpgradeBronze(info)
                            && info.Meat < BUILDING_CENTER_UPGRADE_BRONZEAGE_FOOD;

    // 【发育策略】铜器后优先"复合弓科技"（大弓手）：未研发出来之前，其它耗食物/木头的科技全部让路
    //   —— 目的：第二波前确保能造出大弓手（复合弓兵）
    const bool rushCompositeBow = (bronze
                                   && m_researchCount[BUILDING_RANGE_UPGRADE_COMPOSITE_BOW] == 0
                                   && countBuilding(info, BUILDING_RANGE) > 0);
    if (rushCompositeBow) {
        for (const tagBuilding& b : info.buildings) {
            if (b.Type != BUILDING_RANGE) continue;
            if (b.Percent < 100 || b.Project != ACT_NULL) continue;   // 建造中或忙碌
            if (m_issued.count(b.SN)) continue;
            if (info.Meat >= BUILDING_RANGE_UPGRADE_COMPOSITE_BOW_FOOD
                && info.Wood >= BUILDING_RANGE_UPGRADE_COMPOSITE_BOW_WOOD) {
                BuildingAction(b.SN, BUILDING_RANGE_UPGRADE_COMPOSITE_BOW);
                m_issued.insert(b.SN);
                m_researchCount[BUILDING_RANGE_UPGRADE_COMPOSITE_BOW]++;
            }
            break;
        }
        return;   // 其它科技暂停：资源全部留给复合弓科技
    }
    for (const tagBuilding& b : info.buildings) {
        if (b.Percent < 100 || b.Project != ACT_NULL) continue;   // 建造中或忙碌
        if (m_issued.count(b.SN)) continue;                        // 本帧已下令
        // 攒升级期间科技全停，但**谷仓例外**：建完靶场要补第二座箭塔，必须先研发箭塔科技
        //   （该 case 内部还有"靶场已建 + 塔不足2座 + 石头够"的条件，不会乱花食物）
        if (savingForUpgrade && b.Type != BUILDING_GRANARY) {
            continue;
        }
        switch (b.Type) {
        case BUILDING_GRANARY: {
            // 【用户要求·建完靶场立刻补第二座塔】必须研发箭塔科技（引擎硬前置：Development.cpp:768）
            //   只在"真要建塔"时才花这 50 食：靶场已建 + 塔不足 2 座 + 石头够一座塔(150)
            if (countBuilding(info, BUILDING_RANGE) > 0
                && countBuilding(info, BUILDING_ARROWTOWER) < 2
                && info.Stone >= BUILD_ARROWTOWER_STONE
                && m_researchCount[BUILDING_GRANARY_ARROWTOWER] == 0
                && info.Meat >= BUILDING_GRANARY_ARROWTOWER_FOOD) {
                BuildingAction(b.SN, BUILDING_GRANARY_ARROWTOWER);
                m_issued.insert(b.SN);
                m_researchCount[BUILDING_GRANARY_ARROWTOWER]++;
            }
            break;
        }
        case BUILDING_MARKET: {
            // 【3.0.7g 调整】采集科技在本版才真正生效（旧版整数截断=无效）：
            //   伐木 +50%（1.5倍）、采石/采金 +60%（1.6倍）
            //   → 伐木科技提前：市场建好即研发（木头是市场/靶场=升级瓶颈，加速建设）
            //   → 采石/采金仍等铜器后（箭塔/黄金非升级前置，先省食物攒 800）
            if (m_researchCount[BUILDING_MARKET_WOOD_UPGRADE] == 0
                && info.Meat >= BUILDING_MARKET_WOOD_UPGRADE_FOOD
                && info.Wood >= BUILDING_MARKET_WOOD_UPGRADE_WOOD) {
                BuildingAction(b.SN, BUILDING_MARKET_WOOD_UPGRADE);
                m_issued.insert(b.SN);
                m_researchCount[BUILDING_MARKET_WOOD_UPGRADE]++;
                break;
            }
            // 【发育策略】采石科技停用（不挖石、不造塔/投石兵）→ 省 100 食 + 50 石
            // 【发育策略】车轮科技停用（不出四马战车/战车弓兵）→ 省 150 食 + 100 木
            if (bronze && m_researchCount[BUILDING_MARKET_GOLD_UPGRADE] == 0
                && info.Meat >= BUILDING_MARKET_GOLD_UPGRADE_FOOD
                && info.Wood >= BUILDING_MARKET_GOLD_UPGRADE_WOOD) {
                BuildingAction(b.SN, BUILDING_MARKET_GOLD_UPGRADE);
                m_issued.insert(b.SN);
                m_researchCount[BUILDING_MARKET_GOLD_UPGRADE]++;
                break;
            }
            break;
        }
        case BUILDING_STOCK: {
            // 【3.0.7g 新策略】主力兵种科技是否已解锁（阔剑 或 复合弓）——未解锁前护甲科技让位
            const bool unitTechReady = (m_researchCount[BUILDING_ARMYCAMP_UPGRADE_BROADSWORD] > 0
                                        || m_researchCount[BUILDING_RANGE_UPGRADE_COMPOSITE_BOW] > 0);
            // 工具使用（近战攻击+2）→ 金属加工（铜器，攻击再+2）
            int ut = m_researchCount[BUILDING_STOCK_UPGRADE_USETOOL];
            if (ut == 0 && info.Meat >= BUILDING_STOCK_UPGRADE_CLOSER_ATTACK_FOOD) {
                BuildingAction(b.SN, BUILDING_STOCK_UPGRADE_USETOOL);
                m_issued.insert(b.SN);
                m_researchCount[BUILDING_STOCK_UPGRADE_USETOOL]++;
                break;
            }
            if (bronze && ut == 1 && info.Meat >= BUILDING_STOCK_UPGRADE_CLOSER_ATTACK_2_FOOD
                && info.Gold >= BUILDING_STOCK_UPGRADE_CLOSER_ATTACK_2_GOLD) {
                BuildingAction(b.SN, BUILDING_STOCK_UPGRADE_USETOOL);
                m_issued.insert(b.SN);
                m_researchCount[BUILDING_STOCK_UPGRADE_USETOOL]++;
                break;
            }
            // 步兵护甲
            // 【3.0.7g 新策略】护甲科技让位：先保证主力兵种科技（阔剑/复合弓）解锁，食物紧张
            if (bronze && unitTechReady
                && m_researchCount[BUILDING_STOCK_UPGRADE_DEFENSE_INFANTRY] == 0
                && info.Meat >= BUILDING_STOCK_UPGRADE_DEFENSE_INFANTRY_FOOD) {
                BuildingAction(b.SN, BUILDING_STOCK_UPGRADE_DEFENSE_INFANTRY);
                m_issued.insert(b.SN);
                m_researchCount[BUILDING_STOCK_UPGRADE_DEFENSE_INFANTRY]++;
                break;
            }
            // 弓兵护甲
            if (bronze && unitTechReady
                && m_researchCount[BUILDING_STOCK_UPGRADE_DEFENSE_ARCHER] == 0
                && info.Meat >= BUILDING_STOCK_UPGRADE_DEFENSE_ARCHER_FOOD) {
                BuildingAction(b.SN, BUILDING_STOCK_UPGRADE_DEFENSE_ARCHER);
                m_issued.insert(b.SN);
                m_researchCount[BUILDING_STOCK_UPGRADE_DEFENSE_ARCHER]++;
                break;
            }
            // 骑兵护甲
            if (bronze && unitTechReady
                && m_researchCount[BUILDING_STOCK_UPGRADE_DEFENSE_RIDER] == 0
                && info.Meat >= BUILDING_STOCK_UPGRADE_DEFENSE_RIDER_FOOD) {
                BuildingAction(b.SN, BUILDING_STOCK_UPGRADE_DEFENSE_RIDER);
                m_issued.insert(b.SN);
                m_researchCount[BUILDING_STOCK_UPGRADE_DEFENSE_RIDER]++;
                break;
            }
            // 青铜盾（铜器）
            if (bronze && m_researchCount[BUILDING_STOCK_UPGRADE_MISSILE_DEFENSE_INFANTRY] == 0
                && info.Meat >= BUILDING_STOCK_UPGRADE_MISSILE_DEFENSE_INFANTRY_FOOD
                && info.Gold >= BUILDING_STOCK_UPGRADE_MISSILE_DEFENSE_INFANTRY_GOLD) {
                BuildingAction(b.SN, BUILDING_STOCK_UPGRADE_MISSILE_DEFENSE_INFANTRY);
                m_issued.insert(b.SN);
                m_researchCount[BUILDING_STOCK_UPGRADE_MISSILE_DEFENSE_INFANTRY]++;
                break;
            }
            break;
        }
        case BUILDING_ARMYCAMP: {
            // 阔剑科技（铜器，解锁阔剑兵）
            if (bronze && m_researchCount[BUILDING_ARMYCAMP_UPGRADE_BROADSWORD] == 0
                && info.Meat >= BUILDING_ARMYCAMP_UPGRADE_BROADSWORD_FOOD
                && info.Gold >= BUILDING_ARMYCAMP_UPGRADE_BROADSWORD_GOLD) {
                BuildingAction(b.SN, BUILDING_ARMYCAMP_UPGRADE_BROADSWORD);
                m_issued.insert(b.SN);
                m_researchCount[BUILDING_ARMYCAMP_UPGRADE_BROADSWORD]++;
                break;
            }
            break;
        }
        case BUILDING_RANGE: {
            // 复合弓科技（铜器，解锁复合弓兵）
            if (bronze && m_researchCount[BUILDING_RANGE_UPGRADE_COMPOSITE_BOW] == 0
                && info.Meat >= BUILDING_RANGE_UPGRADE_COMPOSITE_BOW_FOOD
                && info.Wood >= BUILDING_RANGE_UPGRADE_COMPOSITE_BOW_WOOD) {
                BuildingAction(b.SN, BUILDING_RANGE_UPGRADE_COMPOSITE_BOW);
                m_issued.insert(b.SN);
                m_researchCount[BUILDING_RANGE_UPGRADE_COMPOSITE_BOW]++;
                break;
            }
            break;
        }
        default: break;
        }
    }
}

// ============================================================
// 训练军队（铜器后按 PPT 规划）
// 兵营：棍棒兵 → 阔剑兵(需阔剑科技)
// 靶场：弓箭手 → 复合弓兵(需复合弓科技)
// 马厩：侦察骑兵 → 骑兵(铜器)
// 学院：方阵兵(铜器)
// ============================================================
void UsrAI::trainArmy(const tagInfo& info)
{
    if (info.Human_Num >= info.Human_MaxNum) return;   // 人口已满
    bool bronze = (info.civilizationStage >= CIVILIZATION_BRONZEAGE);

    for (const tagBuilding& b : info.buildings) {
        if (b.Percent < 100 || b.Project != ACT_NULL) continue;   // 建造中或忙碌
        if (m_issued.count(b.SN)) continue;                        // 本帧已下令
        switch (b.Type) {
        case BUILDING_ARMYCAMP:
            // 【发育策略】不造弱兵：棍棒兵一律不造（第一波靠祭司转化 + 开局箭塔）
            //   铜器后 + 阔剑科技 → 阔剑兵（35食+15金）
            if (bronze && m_researchCount[BUILDING_ARMYCAMP_UPGRADE_BROADSWORD] > 0
                && info.Meat >= BUILDING_ARMYCAMP_CREATE_BROADSWORD_FOOD && info.Gold >= 15) {
                BuildingAction(b.SN, BUILDING_ARMYCAMP_CREATE_BROADSWORD);
                m_issued.insert(b.SN);
            }
            break;
        case BUILDING_RANGE:
            // 【用户要求·3.0.7g】升级铜器期间（市中心正在升级）先造 **2 个弓箭手**应急：
            //   弓箭手 40 食 + 20 木、不需要科技、不花黄金 → 正好填上"升级期完全没兵"的空档，
            //   第一波转化来的部队万一被打掉也不至于防线全空。
            //   （存活数 < 2 就补，升级期间被打死了会自动补回 2 个；升完铜器立刻转大弓手）
            if (!bronze && m_bronzeUpgradeFrame >= 0
                && countArmy(info, AT_BOWMAN) < 2
                && info.Meat >= BUILDING_RANGE_CREATE_BOWMAN_FOOD
                && info.Wood >= BUILDING_RANGE_CREATE_BOWMAN_WOOD) {
                BuildingAction(b.SN, BUILDING_RANGE_CREATE_BOWMAN);
                m_issued.insert(b.SN);
                break;
            }
            // 【修复·无兵空窗】铜器后、复合弓还没成规模时，先用**便宜的普通弓箭手**垫兵力
            //   实测 15 分钟 army 只有 1 ✗：铜器后只等复合弓（180 食科技 + 20 金/个），
            //   经济薄弱时这 180 食很晚才凑齐 → 中间完全没有兵 ✗ → 敌人一到就崩
            //   普通弓箭手只要 40 食 + 20 木、不需要科技 ✓（顺便满足探路的兵力门槛）
            if (bronze
                && countArmy(info, AT_BOWMAN) + countArmy(info, AT_COMPOSITE_BOWMAN) < 3
                && info.Meat >= BUILDING_RANGE_CREATE_BOWMAN_FOOD
                && info.Wood >= BUILDING_RANGE_CREATE_BOWMAN_WOOD) {
                BuildingAction(b.SN, BUILDING_RANGE_CREATE_BOWMAN);
                m_issued.insert(b.SN);
                break;
            }
            // 铜器后 → 大弓手（复合弓兵，确保造出）
            if (bronze && m_researchCount[BUILDING_RANGE_UPGRADE_COMPOSITE_BOW] > 0
                && info.Meat >= BUILDING_RANGE_CREATE_COMPOSITE_BOWMAN_FOOD && info.Gold >= 20) {
                BuildingAction(b.SN, BUILDING_RANGE_CREATE_COMPOSITE_BOWMAN);
                m_issued.insert(b.SN);
            }
            break;
        case BUILDING_STABLE:
            // 【发育策略】不造侦察骑兵（100 食物太贵，探路靠祭司）→ 直接出骑兵
            //   骑兵：70食+80金，150血/速度4，克步兵且能救祭司
            if (bronze && info.Meat >= BUILDING_STABLE_CREATE_CAVALRY_FOOD && info.Gold >= 80) {
                BuildingAction(b.SN, BUILDING_STABLE_CREATE_CAVALRY);
                m_issued.insert(b.SN);
            }
            break;
        case BUILDING_COLLAGE:
            // 【3.0.7g 新策略】方阵兵（60食+40金，120血/17攻，正面肉盾）——金矿翻倍后负担得起
            if (bronze && info.Meat >= BUILDING_COLLAGE_CREATE_HOPLITE_FOOD && info.Gold >= 40) {
                BuildingAction(b.SN, BUILDING_COLLAGE_CREATE_HOPLITE);
                m_issued.insert(b.SN);
            }
            break;
        default: break;
        }
    }
}

// ============================================================
// 建造第二座箭塔：与第一座塔相距约 6 格，形成交叉火力
// 祭司站两塔中点，敌人从任何方向来都会被至少一座塔覆盖
// ============================================================
void UsrAI::buildArrowTower(const tagInfo& info)
{
    // 前置：谷仓箭塔科技已研发
    if (m_researchCount[BUILDING_GRANARY_ARROWTOWER] == 0) return;

    // 统计已有箭塔（含建造中）
    int towerX = -1, towerY = -1, towerCount = 0;
    for (const tagBuilding& b : info.buildings) {
        if (b.Type != BUILDING_ARROWTOWER) continue;
        towerCount++;
        if (towerCount == 1) { towerX = b.BlockDR; towerY = b.BlockUR; }
    }
    // 【3.0.7g 新策略】箭塔上限 2 座（旧策略 3 座）：
    //   本版初始石头仅 150（=1座塔），且第三波有 2 辆投石车（射程10 > 塔射程8）专拆塔
    //   → 塔多反成负担；省下的石头/人力投入科技与精兵
    // 【发育策略】开局地图已自带 1 座箭塔 → 不再额外造塔（原本的"补第二/三座塔"逻辑停用）
    if (towerCount >= 1) return;
    if (info.Stone < BUILD_ARROWTOWER_STONE) return;  // 石头不足

    // 找一个空闲农民来建造
    for (const tagFarmer& f : info.farmers) {
        if (f.FarmerSort != FARMERTYPE_FARMER) continue;
        if (f.NowState != HUMAN_STATE_IDLE) continue;
        if (m_issued.count(f.SN)) continue;

        // 第二座塔：第一座 +2 格（远离市中心方向）；第三座塔：第一座 -2 格（另一侧）
        // 三座一字排开、间距 2 格（近，交叉火力覆盖）
        int dirX = 1;
        if (m_centerX >= 0 && towerX > m_centerX) dirX = -1;   // 远离市中心方向
        int side = (towerCount == 1) ? 1 : -1;                 // 第二座正向、第三座反向
        int baseX = towerX + dirX * 2 * side;
        int candY[3] = { towerY, towerY - 2, towerY + 2 };     // 正对 / 上挪2格 / 下挪2格
        for (int k = 0; k < 3; ++k) {
            for (int dx = -2; dx <= 2; ++dx) {                 // 附近 ±2 格挪动
                for (int dy = -2; dy <= 2; ++dy) {
                    int bx = baseX + dx, by = candY[k] + dy;
                    if (bx < 0 || by < 0 || bx + 2 > 100 || by + 2 > 100) continue;
                    bool ok = true;
                    for (int i = 0; i < 2 && ok; ++i)
                        for (int j = 0; j < 2; ++j)
                            if (m_map[bx + i][by + j] != 0) { ok = false; break; }
                    // 高度一致（平地）
                    if (ok && info.theMap != nullptr) {
                        const auto& terrain = *info.theMap;
                        int h = terrain[bx][by].height;
                        if (terrain[bx + 1][by].height != h || terrain[bx][by + 1].height != h
                            || terrain[bx + 1][by + 1].height != h) ok = false;
                    }
                    if (ok) {
                        HumanBuild(f.SN, BUILDING_ARROWTOWER, bx, by);
                        m_issued.insert(f.SN);
                        return;   // 建好直接结束
                    }
                }
            }
        }
        break;
    }
}

// 计算祭司站位：守在"远离敌人来袭方向的那座塔"下（任意塔数；无塔 → 市中心）
void UsrAI::getPriestHome(const tagInfo& info, int& hx, int& hy) const
{
    hx = m_centerX;
    hy = m_centerY;
    int bestX = -1, bestY = -1;
    double bestProj = 1e18;
    for (const tagBuilding& b : info.buildings) {
        if (b.Type != BUILDING_ARROWTOWER || b.Percent < 100) continue;
        if (m_enemyDirX != 0 || m_enemyDirY != 0) {
            // 选"离敌人来袭方向最远"的塔（塔相对市中心的投影最小 = 敌人反侧，最安全）
            double d = (double)(b.BlockDR - m_centerX) * m_enemyDirX
                     + (double)(b.BlockUR - m_centerY) * m_enemyDirY;
            if (d < bestProj) { bestProj = d; bestX = b.BlockDR; bestY = b.BlockUR; }
        } else if (bestX < 0) {
            bestX = b.BlockDR; bestY = b.BlockUR;   // 未知敌人方向：选第一座塔
        }
    }
    if (bestX >= 0) { hx = bestX; hy = bestY; }     // 有塔 → 选好的塔下
}

// ============================================================
// 【修复·祭司拉怪】算"撤退终点"：塔坐标 + **背对追兵偏移 1.5 格**
//   为什么必须偏移：祭司停在**塔中心**时，射程 7 的战车弓兵会停在离祭司 7 格处
//   = 离塔 7~8 格，而箭塔索敌是 defense() 里 if (dt > range) continue（range=7）
//   → 正好锁不到它，拉怪白拉。
//   站到"塔背对追兵"的一侧后，追兵要打到祭司就必须绕到那一侧，
//   距塔变成 7-1.5 ≈ 5.5 格 < 7 → 箭塔锁得到 → 仇恨从祭司转到塔上。
//   参数：(ex,ey)=追兵（第二个战车弓兵/最近威胁）位置；(hx,hy)=选中的塔块坐标
//   文件作用域 static 自由函数 —— **不进类**，保证 sizeof(UsrAI) 不变。
// ============================================================
static void priestRetreatPoint(double ex, double ey, int hx, int hy, double& gx, double& gy)
{
    gx = (double)hx * BLOCKSIDELENGTH;
    gy = (double)hy * BLOCKSIDELENGTH;
    double vx = gx - ex, vy = gy - ey;              // 从追兵指向塔
    double len = sqrt(vx * vx + vy * vy);
    if (len < 1e-6) { vx = 0.0; vy = 1.0; len = 1.0; }
    const double OFFSET = 1.5 * BLOCKSIDELENGTH;    // 再沿同方向外移 1.5 格 = 塔背面
    gx += vx / len * OFFSET;
    gy += vy / len * OFFSET;
}

// ============================================================
// 【修复·祭司来回打转】锁存版撤退点
//   priestRetreatPoint 的输出跟着"追兵当前位置"每帧变，而 movePriest 里
//   "目标差 >2 格就绕过 60 帧节流"（:2274）→ 变成每 15 帧一条 HumanMove
//   → 祭司绕着塔背面抽搐。这里把点锁存起来：只有
//     ① 首次  ② 已到位(≤1.5 格)  ③ 距上次重算 ≥120 帧
//   才重新计算，其余帧沿用同一个点 → 节流恢复正常 → 走一段、停一段（不再抽）。
//   slot：0=拉怪走位，1=威胁撤退（两处各自独立锁存）
//   （文件作用域 static，不进类）
// ============================================================
static void priestRetreatLatched(int slot, double ex, double ey, int hx, int hy,
                                 double px, double py, int frame, double& gx, double& gy)
{
    if (slot < 0 || slot > 1) slot = 0;
    const double dx = px - (double)m_retGX[slot];
    const double dy = py - (double)m_retGY[slot];
    const double arrive = 1.5 * BLOCKSIDELENGTH;
    const bool needNew = (m_retGX[slot] < 0)
                      || (frame - m_retGF[slot] >= 120)
                      || (dx * dx + dy * dy <= arrive * arrive);
    if (needNew) {
        priestRetreatPoint(ex, ey, hx, hy, gx, gy);
        m_retGX[slot] = (int)gx;
        m_retGY[slot] = (int)gy;
        m_retGF[slot] = frame;
        return;
    }
    gx = (double)m_retGX[slot];
    gy = (double)m_retGY[slot];
}

// ============================================================
// 防守：箭塔"拉仇恨"——优先攻击满血（未标记）的敌人
// 规则：
//   ① 射程内优先选"满血"敌人（标记它/拉到仇恨，广覆盖每个进射程的敌人）
//   ② 满血敌人选"威胁祭司的"优先，其次近塔的
//   ③ 范围内没有满血了 → 才打已掉血的
//   ④ 切换节流：塔切换目标后 30 帧内不切换（防频繁切换导致塔永远不射击）
// ============================================================
void UsrAI::defense(const tagInfo& info)
{
    // 【修复·部队来回乱晃】反攻启动后，部队归 attackPhase 全权指挥 ✓
    //   否则下面"③ 无战事 → 回塔集结"会把已经走到集结点的兵又叫回家 ✗
    //   → 兵在家 ↔ 集结点之间无限往返 = "一大堆兵在那乱晃" ✓✓
    //   反攻期间家里交给箭塔防守 ✓（这就是反攻模块原本的设计意图 ✓）
    if (m_atkOn) return;
    double range = DIS_ARROWTOWER * BLOCKSIDELENGTH;    // 箭塔攻击距离（细节单位）

    // 记录敌人来袭方向（首次发现敌人时，供祭司站位偏移用）
    if (m_enemyDirX == 0 && m_enemyDirY == 0 && !info.enemy_armies.empty()) {
        double ex = 0, ey = 0;
        int cnt = 0;
        for (const tagArmy& e : info.enemy_armies) { ex += e.DR; ey += e.UR; cnt++; }
        if (cnt > 0) {
            double dx = ex / cnt - (double)m_centerX * BLOCKSIDELENGTH;
            double dy = ey / cnt - (double)m_centerY * BLOCKSIDELENGTH;
            m_enemyDirX = (dx > 0) ? 1 : -1;
            m_enemyDirY = (dy > 0) ? 1 : -1;
        }
    }

    // 找祭司（用于判断谁在威胁祭司）
    const tagArmy* priest = nullptr;
    for (const tagArmy& a : info.armies) {
        if (a.Sort == AT_PRIEST) { priest = &a; break; }
    }

    for (const tagBuilding& b : info.buildings) {
        if (b.Type != BUILDING_ARROWTOWER) continue;
        if (b.Percent < 100) continue;                  // 建造中
        if (m_issued.count(b.SN)) continue;             // 本帧已下令

        double towerDR = (double)b.BlockDR * BLOCKSIDELENGTH;
        double towerUR = (double)b.BlockUR * BLOCKSIDELENGTH;

        // 射程内选目标：满血（未标记）优先，其次掉血的
        int fullTarget = -1, weakTarget = -1;
        double bestFullD = 1e18, bestFullPriest = 1e18;
        double bestWeakD = 1e18, bestWeakPriest = 1e18;
        for (const tagArmy& e : info.enemy_armies) {
            double dt = calDistance(towerDR, towerUR, e.DR, e.UR);
            if (dt > range) continue;
            double dp = (priest != nullptr) ? calDistance(priest->DR, priest->UR, e.DR, e.UR) : 1e18;
            if (e.Blood >= e.MaxBlood) {                        // 满血 = 未标记 → 拉仇恨
                bool better = false;
                if (fullTarget < 0) better = true;
                else if (dp < bestFullPriest - 1.0) better = true;
                else if (dp <= bestFullPriest + 1.0 && dt < bestFullD) better = true;
                if (better) { fullTarget = e.SN; bestFullPriest = dp; bestFullD = dt; }
            } else {                                            // 已掉血 = 已标记
                bool better = false;
                if (weakTarget < 0) better = true;
                else if (dp < bestWeakPriest - 1.0) better = true;
                else if (dp <= bestWeakPriest + 1.0 && dt < bestWeakD) better = true;
                if (better) { weakTarget = e.SN; bestWeakPriest = dp; bestWeakD = dt; }
            }
        }
        int target = (fullTarget >= 0) ? fullTarget : weakTarget;
        if (target < 0) continue;

        // 判断塔当前攻击目标是否已掉血（被命中过）
        bool curHit = false;
        if (b.Project > 0) {
            for (const tagArmy& e : info.enemy_armies) {
                if (e.SN == b.Project) { curHit = (e.Blood < e.MaxBlood); break; }
            }
        }

        if (b.Project <= 0) {
            // 空闲 → 立即锁定目标
            HumanAction(b.SN, target);
            m_issued.insert(b.SN);
            m_towerSwitch[b.SN] = info.GameFrame;
        } else if (curHit) {
            // 已命中当前目标（掉血）→ 切换射程内"下一个角色"（排除当前目标）
            // 逐个点名范围内敌人，不限满血（满血优先，其次掉血）
            int nextTarget = -1;
            if (fullTarget >= 0 && fullTarget != b.Project) nextTarget = fullTarget;
            else if (weakTarget >= 0 && weakTarget != b.Project) nextTarget = weakTarget;
            if (nextTarget >= 0) {
                auto it = m_towerSwitch.find(b.SN);
                if (it == m_towerSwitch.end() || info.GameFrame - it->second >= 30) {
                    HumanAction(b.SN, nextTarget);
                    m_issued.insert(b.SN);
                    m_towerSwitch[b.SN] = info.GameFrame;
                }
            }
        }
        // 否则：继续打当前目标（范围内没有其他敌人时专注打死）
    }

    // ===== 军队：布防与迎击（保护祭司，提前锁定远程威胁） =====
    // 布防点：双塔中点（保护祭司站位）> 市中心
    int hx, hy;
    getPriestHome(info, hx, hy);
    // 【修复·部队原地抽动】原目标 = 块**角点** `(double)hx * BSL` ✗
    //   角点常常正是建筑格/树格（不可达 ✗）→ 部队永远进不了容差 → **每帧重下一条移动令** ✗
    //   改成：从家块向外找第一个"可站立格子"（isStaticBlock ✓），用它的**中心**当集结点 ✓
    int homeBX = hx, homeBY = hy;
    {
        bool found = false;
        for (int rr = 0; rr <= 4 && !found; ++rr) {
            for (int ddx = -rr; ddx <= rr && !found; ++ddx) {
                for (int ddy = -rr; ddy <= rr && !found; ++ddy) {
                    if (rr > 0 && ddx > -rr && ddx < rr && ddy > -rr && ddy < rr) continue;  // 只扫这一圈
                    const int bx = hx + ddx, by = hy + ddy;
                    if (bx < 1 || bx > MAP_L - 2 || by < 1 || by > MAP_U - 2) continue;
                    if (isStaticBlock(bx, by)) continue;      // 树/矿/建筑/海洋 → 不能站 ✗
                    homeBX = bx; homeBY = by; found = true;
                }
            }
        }
    }
    double homeDR = ((double)homeBX + 0.5) * BLOCKSIDELENGTH;   // ★用格心（原来用角点 ✗）
    double homeUR = ((double)homeBY + 0.5) * BLOCKSIDELENGTH;
    // 【修复·探路兵把全军拉走】原来 enemyVisible = "快照里有任何敌人" ✗
    //   而敌方**单位**是按 getvisible() 过滤进快照的（Core.cpp:479/505）——
    //   探路兵跑到敌营附近就会把敌方部队"点亮" → 触发下面的全军迎击分支
    //   （战车弓优先 / 其它远程 / 最近敌人，全都没有距离限制）→ 所有兵跨全图去打。
    //   现在：只有"离布防点 DEFEND_R 格内"的敌人才算来犯。
    //   第一~三波的敌人是冲基地来的，必进此圈 → 防守行为不变 ✓
    const double DEFEND_R = 22.0 * BLOCKSIDELENGTH;   // 迎击半径（格），可调
    bool enemyVisible = false;
    for (const tagArmy& e : info.enemy_armies) {
        if (calDistance(e.DR, e.UR, homeDR, homeUR) <= DEFEND_R) { enemyVisible = true; break; }
    }
    if (!enemyVisible) {
        for (const tagFarmer& e : info.enemy_farmers) {
            if (calDistance(e.DR, e.UR, homeDR, homeUR) <= DEFEND_R) { enemyVisible = true; break; }
        }
    }

    for (const tagArmy& a : info.armies) {
        if (a.Sort == AT_PRIEST || a.Sort == AT_SCOUT) continue;   // 祭司/侦察骑兵单独调度
        // 【跨模块·豁免】我方派出去探路的那个兵不受 defense 调度 ✗
        //   否则它一到路点（帧首快照里是 IDLE）就被"回塔集结"叫回家 ✗
        if (m_scoutUnitSN >= 0 && !m_scoutDone && a.SN == m_scoutUnitSN) continue;
        if (m_issued.count(a.SN)) continue;                         // 本帧已下令

        if (enemyVisible) {
            // 0) 保祭司转火：谁正在打祭司就打谁（第三波战车弓/四马战车/复合弓都可能集火祭司）
            //    判定：敌人 WorkObjectSN == 我方祭司 SN → 它正在攻击祭司
            //    其次：视野内的祭司特攻单位（战车弓兵/四马战车，对祭司+7）——潜在杀祭司威胁
            //    分散攻击：统计候选已被几个己方兵锁定 → 优先打"被攻击最少"的，
            //    避免全军集火一个、另一个无人拉仇恨继续打祭司
            int priestSN = (priest != nullptr) ? priest->SN : -1;
            // 候选集合：正在打祭司的敌人 > 祭司特攻单位（战车弓/四马战车）
            std::vector<int> candidates;
            if (priestSN >= 0) {
                for (const tagArmy& e : info.enemy_armies)
                    if (e.WorkObjectSN == priestSN) { candidates.push_back(e.SN); break; }
            }
            if (candidates.empty()) {
                // 【3.0.7g 新策略】祭司特攻单位 + 投石车：
                //   战车弓/四马战车对祭司 +7；投石车射程10 > 塔射程8，第三波 2 辆专拆塔
                //   → 都是必须先杀的高危目标
                for (const tagArmy& e : info.enemy_armies)
                    if (e.Sort == AT_CHARIOT_ARCHER || e.Sort == AT_CHARIOT
                        || e.Sort == AT_STONE_THROWER)
                        candidates.push_back(e.SN);
            }
            if (!candidates.empty()) {
                // 统计每个候选正被几个己方兵锁定
                std::unordered_map<int,int> candLocked;
                for (const tagArmy& my : info.armies) {
                    if (my.Sort == AT_PRIEST || my.Sort == AT_SCOUT) continue;
                    if (my.WorkObjectSN <= 0) continue;
                    for (int csn : candidates)
                        if (csn == my.WorkObjectSN) { candLocked[csn]++; break; }
                }
                int savior = -1;
                int saviorCnt = 0x7fffffff;
                double saviorD = 1e18;
                for (int csn : candidates) {
                    const tagArmy* ce = nullptr;
                    for (const tagArmy& e : info.enemy_armies)
                        if (e.SN == csn) { ce = &e; break; }
                    if (ce == nullptr) continue;
                    int locked = candLocked[csn];
                    double d = calDistance(a.DR, a.UR, ce->DR, ce->UR);
                    if (locked < saviorCnt || (locked == saviorCnt && d < saviorD)) {
                        saviorCnt = locked;
                        saviorD = d;
                        savior = csn;
                    }
                }
                if (savior >= 0) {
                    // 当前正在打它 → 不打断
                    bool already = false;
                    for (const tagArmy& e : info.enemy_armies)
                        if (e.SN == a.WorkObjectSN) { already = true; break; }
                    if (!already) {
                        auto it = m_armySwitch.find(a.SN);
                        if (it == m_armySwitch.end() || info.GameFrame - it->second >= 30) {
                            HumanAction(a.SN, savior);          // 转火救祭司
                            m_issued.insert(a.SN);
                            m_armySwitch[a.SN] = info.GameFrame;
                            continue;
                        }
                    }
                    continue;   // 已在打 → 本帧不管
                }
            }
        }
        if (a.NowState != HUMAN_STATE_IDLE) continue;               // 已在战斗的不重复下令
        if (m_issued.count(a.SN)) continue;

        if (enemyVisible) {
            // 【用户要求·第二三波】小兵**分散**去拉不同远程兵的仇恨（一个兵对一个远程兵 ✓），
            //   近战兵交给箭塔处理（小兵不去追远处近战 ✗）
            //   做法与上面"救祭司"那段一致：统计每个候选已被几个己方兵锁定，
            //   优先打"被锁最少"的（同数则取最近）→ 自然分散 ✓
            std::vector<int> picks;
            for (const tagArmy& e : info.enemy_armies)
                if (e.Sort == AT_CHARIOT_ARCHER) picks.push_back(e.SN);       // ① 战车弓最优先
            if (picks.empty())
                for (const tagArmy& e : info.enemy_armies)
                    if (e.Sort == AT_STONE_THROWER || e.Sort == AT_COMPOSITE_BOWMAN
                        || e.Sort == AT_BOWMAN || e.Sort == AT_SLINGER)
                        picks.push_back(e.SN);                                // ② 其他远程
            int target = -1;
            if (!picks.empty()) {
                std::unordered_map<int,int> pickLocked;
                for (const tagArmy& my : info.armies) {
                    if (my.Sort == AT_PRIEST || my.Sort == AT_SCOUT) continue;
                    if (my.WorkObjectSN <= 0) continue;
                    for (int psn : picks)
                        if (psn == my.WorkObjectSN) { pickLocked[psn]++; break; }
                }
                int bestCnt = 0x7fffffff;
                double bestD = 1e18;
                for (int psn : picks) {
                    const tagArmy* pe = nullptr;
                    for (const tagArmy& e : info.enemy_armies)
                        if (e.SN == psn) { pe = &e; break; }
                    if (pe == nullptr) continue;
                    const int lk = pickLocked[psn];
                    const double d = calDistance(a.DR, a.UR, pe->DR, pe->UR);
                    if (lk < bestCnt || (lk == bestCnt && d < bestD)) {
                        bestCnt = lk; bestD = d; target = psn;
                    }
                }
            }
            // ③ 没有远程目标 → 只打**贴到 6 格内**的敌人（6 < 箭塔射程 7 ✓）
            //   远处的近战交给箭塔，小兵不追 ✗（避免白白送人头 ✓）
            if (target < 0) {
                double best = 6.0 * BLOCKSIDELENGTH;
                for (const tagArmy& e : info.enemy_armies) {
                    const double d = calDistance(a.DR, a.UR, e.DR, e.UR);
                    if (d < best) { best = d; target = e.SN; }
                }
                if (target < 0) {
                    best = 6.0 * BLOCKSIDELENGTH;
                    for (const tagFarmer& e : info.enemy_farmers) {
                        const double d = calDistance(a.DR, a.UR, e.DR, e.UR);
                        if (d < best) { best = d; target = e.SN; }
                    }
                }
            }
            if (target >= 0) {
                HumanAction(a.SN, target);
                m_issued.insert(a.SN);
            }
        } else {
            // ③ 无战事 → 集结到布防点（守在祭司/基地周围，敌人来袭时能提前锁定）
            // 【修复·重复下令】容差 5 → 7 格（挤在一起也能算"到了"✓）+ 同一兵 90 帧内不重下 ✗
            if (calDistance(a.DR, a.UR, homeDR, homeUR) > 7.0 * BLOCKSIDELENGTH) {
                std::map<int,int>::iterator rit = m_homeRecallFrame.find(a.SN);
                const int lastRecall = (rit == m_homeRecallFrame.end()) ? -99999 : rit->second;
                if (info.GameFrame - lastRecall >= 90) {          // 3.6 秒才允许重下一次 ✓
                    HumanMove(a.SN, homeDR, homeUR);
                    m_issued.insert(a.SN);
                    m_homeRecallFrame[a.SN] = info.GameFrame;
                }
            } else {
                m_homeRecallFrame.erase(a.SN);                    // 到位了 → 清记录 ✓
            }
        }
    }
}

// ============================================================
// 某格是否"静态障碍"（不能作为移动目标）：树/石/金/浆果丛/建筑/海洋
//   单位（200=我方 300=敌方）不算障碍 —— 祭司要靠近转化目标，可下达
//   动物资源不算障碍 —— 会移动，且可能被采集/猎杀
//   未探索(-2) 按可走处理 —— 避免目标永远不可达
// ============================================================
bool UsrAI::isStaticBlock(int bx, int by) const
{
    if (bx < 0 || bx >= 100 || by < 0 || by >= 100) return true;
    int v = m_map[bx][by];
    if (v == -1) return true;                 // 海洋
    if (v >= 100 && v < 200) return true;     // 建筑（100+类型）
    if (v >= 200) return false;               // 单位（敌我）可下达
    if (v >= 10 && v < 100) {                 // 资源（10+类型）
        int t = v - 10;
        if (t == RESOURCE_TREE || t == RESOURCE_STONE
            || t == RESOURCE_GOLD || t == RESOURCE_BUSH) return true;  // 静态障碍
        return false;                         // 动物（羚羊/大象/狮子）可走
    }
    return false;                             // 0 空地、-2 未探索：可走
}

// ============================================================
// 目标格是静态障碍（如树木）→ 调整到周围最近的可达格
//   环形搜索半径 1..8 格（曼哈顿距离优先），找到即返回
//   全被堵则保持原目标（放弃调整，交给游戏寻路处理）
// ============================================================
void UsrAI::adjustReachableTarget(double& gx, double& gy) const
{
    int bx = (int)(gx / BLOCKSIDELENGTH + 0.5);   // 像素 → 最近块
    int by = (int)(gy / BLOCKSIDELENGTH + 0.5);
    if (!isStaticBlock(bx, by)) return;           // 目标格可下达，不用改

    for (int r = 1; r <= 8; ++r) {                // 逐圈扩大（曼哈顿距离优先）
        for (int dy = -r; dy <= r; ++dy) {
            for (int dx = -r; dx <= r; ++dx) {
                if (dx == 0 && dy == 0) continue;
                int nx = bx + dx, ny = by + dy;
                if (nx < 0 || nx >= 100 || ny < 0 || ny >= 100) continue;
                if (!isStaticBlock(nx, ny)) {
                    gx = (double)nx * BLOCKSIDELENGTH;
                    gy = (double)ny * BLOCKSIDELENGTH;
                    return;
                }
            }
        }
    }
    // 8格内全堵：保持原目标
}

// ============================================================
// 祭司节流移动：防止每帧重复下同一个移动指令
//   问题：AI 每帧给祭司下移动令 → 移动/寻路不断被重置 → 永远到不了目标
//         → 下一帧距离判断仍"没到" → 又下令（死循环，且挤掉转化指令）
//   解决：
//     ① 已到目标 1 格内 → 不下令（到位即停，不再重复发同一坐标）
//     ② 60帧（2.4秒）内目标位置变化不超过 2 格 → 不重复下令
//     ③ 只有目标大范围变化时才立即重新下令（追新目标）
//     ④ 目标格是静态障碍（树木/建筑/海洋）→ 先调整到最近可达格再下达
//   返回 true = 本帧已下令
// ============================================================
bool UsrAI::movePriest(int priestSN, double px, double py, double gx, double gy, int frame)
{
    // ④ 目标格是障碍（如树木挡路）→ 调整到周围最近可达格
    adjustReachableTarget(gx, gy);
    // ① 已到目标 1 格内 → 到位即停
    if (calDistance(px, py, gx, gy) <= 1.0 * BLOCKSIDELENGTH) return false;
    // ② 【修复·反复移动】最小间隔硬闸：任何情况下 15 帧内不再重新下令
    //    原因：原来"目标差 >2 格就绕过 60 帧节流"，当两个模块（回塔待命 / 探路边界）
    //    给出不同目标时，会每帧交替下发 HumanMove → 祭司原地来回抽搐。
    if (frame - m_priestMoveFrame < 15) return false;
    // ③ 节流：60帧内且目标没大变 → 不下令
    bool cool = (frame - m_priestMoveFrame < 60);
    bool targetChanged = calDistance(m_priestMoveDR, m_priestMoveUR, gx, gy) > 2.0 * BLOCKSIDELENGTH;
    if (cool && !targetChanged) return false;

    HumanMove(priestSN, gx, gy);
    m_priestMoveFrame = frame;
    m_priestMoveDR = gx;
    m_priestMoveUR = gy;
    m_issued.insert(priestSN);
    return true;
}

// ============================================================
// 祭司行为：
//   被威胁时：先撤退到"最近的塔旁"（贴塔站位，把敌人拉进塔射程）
//             到位后再考虑转化（不原地站着挨打）
//   无威胁时：转化"正被攻击"的敌人（冷却可用时）
// 几何依据：敌人射程r、祭司贴塔d格 → 敌人距塔=d+r，需 ≤ 塔射程(7)
//           弓箭手r=5 → 祭司需贴塔2格内，敌人追进来就被塔打
// ============================================================
void UsrAI::handlePriest(const tagInfo& info)
{
    // 【修复·祭司不集结】反攻启动后，祭司**全权交给 attackPhase** ✓
    //   为什么必须让位：本函数第 6 步"回塔待命"会每帧把他叫回家 ✗ 并写进 m_issued ✗
    //   而 attackPhase 的祭司分支第一行是 `!m_issued.count(priestSN)` ✗ →
    //   他**永远收不到"去集结点/跟部队"的令** → 表现就是"集结时祭司不集结" ✓✓
    //   （与"探路兵被 defense 抢走"是同一类问题 ✓）
    if (m_atkOn) return;
    // 1) 找到祭司
    int priestSN = -1;
    const tagArmy* priest = nullptr;
    for (const tagArmy& a : info.armies) {
        if (a.Sort == AT_PRIEST) { priestSN = a.SN; priest = &a; break; }
    }
    if (priest == nullptr) return;                  // 祭司不存在（死亡=游戏失败）

    // ===== 【修复·祭司出塔迎击被锁死】"拴绳" =====
    //   转化施法 2~6 秒，期间祭司站着不动；一旦跑出箭塔保护圈就是在空地站 6 秒 ✗
    //   战车弓兵对祭司 +7 特攻，两辆 ≈14.7 DPS → 100 血只够 6.8 秒，正好被卡死 ✓
    //   规则：离最近的箭塔 > PRIEST_LEASH 格 → 立刻回塔（塔没了→市中心），本帧不再做别的
    //   注：反攻阶段不受影响（attackPhase 在 handlePriest 之后调用，同帧最后一条令生效 ✓）
    {
        double nearTowerD = 1e18;
        for (const tagBuilding& tb : info.buildings) {
            if (tb.Type != BUILDING_ARROWTOWER || tb.Percent < 100) continue;
            const double d = calDistance(priest->DR, priest->UR,
                                         (double)tb.BlockDR * BLOCKSIDELENGTH,
                                         (double)tb.BlockUR * BLOCKSIDELENGTH);
            if (d < nearTowerD) nearTowerD = d;
        }
        // 有箭塔 且 离所有塔都太远 → 回塔（无塔时不拴绳，避免把祭司永远钉在原地 ✗）
        if (nearTowerD < 1e17 && nearTowerD > PRIEST_LEASH * BLOCKSIDELENGTH) {
            int lhx, lhy;
            getPriestHome(info, lhx, lhy);
            if (lhx >= 0 && priest->NowState == HUMAN_STATE_IDLE)
                movePriest(priestSN, priest->DR, priest->UR,
                           (double)lhx * BLOCKSIDELENGTH, (double)lhy * BLOCKSIDELENGTH,
                           info.GameFrame);
            return;                 // ★回塔优先：本帧不转化/不迎击/不拉怪
        }
    }

    // 1.5) 被攻击检测：血量比上一帧下降 → 判定正在挨打
    //      走位打断规则（关键：转化不能被打断，否则 2~6 秒施法白费、屡屡不成功）：
    //      · 转化中（convertingNow，或刚下令 30 帧内快照未更新）→ 只有濒死(<25%)才走位打断
    //        —— 转化就是把打自己的敌人转化掉，是最佳自救，坚持走完
    //      · 没在转化 → 低血(<60%) 才走位躲避（保命）
    bool beingHit = (m_priestLastBlood > 0 && priest->Blood < m_priestLastBlood);
    m_priestLastBlood = priest->Blood;
    bool convertingNow = false;
    for (const tagArmy& e : info.enemy_armies)
        if (e.SN == priest->WorkObjectSN) { convertingNow = true; break; }
    // 【修复·"转化半天没成功"】转化施法在引擎里是"随机 2~6 秒"（Core_List.cpp:610-614），
    //   而且**任何移动指令都会 suspendRelation → 转化计时作废、下次重新随机**。
    //   原来这里只保护"刚下令 30 帧（1.2 秒）"，1.2 秒之后第 5 步（威胁撤退）/
    //   第 6 步（回塔待命）就会下 HumanMove，把正在进行的转化打断 → 表现为"转化半天不成功"。
    //   现在：
    //     · 保护窗口放大到 150 帧（= 引擎最大施法 6 秒）
    //     · 一旦转化成功（冷却从 0 变成 >0）立刻清掉窗口 → 用户战术里的"转化后立刻跑位"不受影响
    if (priest->ConvertCooldown > 0 && m_convertStartFrame >= 0) {
        m_convertStartFrame = -1;      // 本次转化已成功 → 退出保护窗口
        m_lastConvertedSN = m_convertTarget;   // 【修复·拉怪】记住"刚转化的那个"：
        //   它已经变成我方单位，但敌人快照可能还残留 1~2 帧。拉怪找"第二个战车弓兵"
        //   时必须排除它，否则会把它当敌人 → 撤退方向算成"远离它" → 正好迎着第二个跑。
    }
    // 快照延迟补偿：刚下令转化（150帧内）主线程快照可能还没把 WorkObjectSN 传回来
    // 【修复·转化中途被打断】窗口 150 → 200 帧：引擎最大施法 150 帧（6 秒）+ 快照延迟，
    //   原来正好卡在上限边界 → 施法快结束时窗口先到期 → 第 3.5/5/6 步下 HumanMove → 作废。
    bool justOrderedConvert = (m_convertStartFrame >= 0
                               && info.GameFrame - m_convertStartFrame < 200);
    bool inConversion = convertingNow || justOrderedConvert;
    // 【修复·转化中途被打断】转化期间每帧把祭司放进 m_issued：
    //   m_issued 在 processData 开头清空，而转化令是上一帧下的 → 本帧并不在集合里，
    //   后面任何模块理论上都能再下一条令把转化打断。这里每帧补上，彻底挡住。
    //   （本函数自己的"濒死撤退"不受影响，它在下面独立判断。）
    if (inConversion) m_issued.insert(priestSN);
    bool lowBlood = (priest->Blood < priest->MaxBlood * 3 / 5);      // <60%
    bool criticalBlood = (priest->Blood < priest->MaxBlood / 4);     // <25% 濒死

    // ================= 【第二波专属·第一波逻辑完全不动】 =================
    //   第二波有 2 个战车弓兵（对祭司 +7 特攻 ≈7.3 DPS/个），它们会隔着塔锁定祭司；
    //   祭司 100 血、速度 2.03（跑不过任何兵）→ 等掉血再反应往往已经来不及。
    //   本块只做两件事，且只在第二波窗口生效：
    //     ① "被远程兵锁定"也算作转化触发条件（不等挨打，见下面 1.6）
    //     ② 转化时优先挑"锁定祭司的远程兵"，其中战车弓兵最优先（血 70，最容易转化成功）
    //   注意：不新增走位分支（走位放在转化之后的 3.5 节），避免把转化挤掉。
    //   想覆盖第三波：把下面的 FRAME_WAVE3 改成 99999。
    const bool wave2Defense = (info.GameFrame > FRAME_WAVE2 - 3000
                               && info.GameFrame <= FRAME_WAVE3);
    int lockedRangedSN = -1;      // 锁定祭司的"远程兵"（含战车弓）
    int lockedAnySN = -1;         // 锁定祭司的任意敌人
    if (wave2Defense) {
        for (const tagArmy& e : info.enemy_armies) {                 // 第一优先：战车弓兵
            if (e.Blood <= 0) continue;
            if (e.WorkObjectSN != priestSN) continue;
            if (e.Sort == AT_CHARIOT_ARCHER) { lockedRangedSN = e.SN; break; }
        }
        if (lockedRangedSN < 0) {
            for (const tagArmy& e : info.enemy_armies) {             // 其次：其它远程兵
                if (e.Blood <= 0) continue;
                if (e.WorkObjectSN != priestSN) continue;
                bool ranged = (e.Sort == AT_BOWMAN || e.Sort == AT_COMPOSITE_BOWMAN
                               || e.Sort == AT_SLINGER);
                if (ranged) { lockedRangedSN = e.SN; break; }
                if (lockedAnySN < 0) lockedAnySN = e.SN;             // 顺带记住近战锁定者
            }
        }
    }

    // 1.6) 【防守策略·用户要求】受到攻击且"当前没在转化" → 立刻开始转化（自卫，不等己方火力锁定）
    //      前提：未在转化 + 冷却已好（游戏20秒）+ 未濒死（濒死优先逃命，见下面走位）
    //      目标：① 正在攻击祭司的敌人 ② 找不到(快照延迟)则射程内最近的敌人
    //      【第二波追加】被远程兵（战车弓/弓兵）锁定时也立刻转化，且优先转化它
    //                  —— 窗口外 lockedRangedSN 恒为 -1，行为与改动前完全一致
    if ((beingHit || lockedRangedSN >= 0) && !inConversion && !criticalBlood
        && priest->ConvertCooldown <= 0) {
        // 节流必须 > 引擎最大施法时间（150 帧 / 6 秒），否则每到 120 帧就重下令
        // → suspendRelation 把上一轮转化作废、重新随机 → 永远转不完
        bool tooSoon = (m_convertStartFrame >= 0
                        && info.GameFrame - m_convertStartFrame < 180);
        if (!tooSoon) {
            // 第二波：先挑"锁定祭司的远程兵"（血只有 35~70，最容易转化成功、威胁也最大）
            int attackerSN = (lockedRangedSN >= 0) ? lockedRangedSN : lockedAnySN;
            if (attackerSN < 0) {
                for (const tagArmy& e : info.enemy_armies) {
                    if (e.Blood <= 0) continue;
                    if (e.WorkObjectSN == priestSN) { attackerSN = e.SN; break; }   // 正在打我
                }
            }
            if (attackerSN < 0) {
                // 快照延迟等原因找不到攻击者 → 用转化射程内最近的敌人
                double bestD = 1e18;
                for (const tagArmy& e : info.enemy_armies) {
                    if (e.Blood <= 0) continue;
                    double d = calDistance(priest->DR, priest->UR, e.DR, e.UR);
                    if (d <= DIS_PRIEST * BLOCKSIDELENGTH && d < bestD) { bestD = d; attackerSN = e.SN; }
                }
            }
            if (attackerSN >= 0) {
                HumanAction(priestSN, attackerSN);   // 立刻转化（自卫）
                m_issued.insert(priestSN);
                m_convertTarget = attackerSN;
                m_convertStartFrame = info.GameFrame;
                return;
            }
        }
    }

    if (beingHit && !info.enemy_armies.empty()
        && (criticalBlood || (!inConversion && lowBlood))) {
        // 走位目标 = 固定安全位（塔下/市中心，getPriestHome）：挨打就往安全位撤，到位即停
        // （不用"塔+敌人反方向偏移"——敌人位置每帧变 → 目标抖动 → 每帧重新下令）
        int hx, hy;
        getPriestHome(info, hx, hy);
        double gx = (double)hx * BLOCKSIDELENGTH;
        double gy = (double)hy * BLOCKSIDELENGTH;
        // 节流下令：60帧内目标不变不重复下令（防每帧打断移动/挤掉转化指令）
        bool ordered = movePriest(priestSN, priest->DR, priest->UR, gx, gy, info.GameFrame);
        if (ordered || criticalBlood) return;   // 已下令，或濒死 → 本帧不再转化
        // 转化中被打但没到濒死：不 return → 把本帧让给转化逻辑（转化继续走完）
    }

    // 2) 寻找转化候选（分阶段策略）
    //    · 8000 帧前（第一波前后，兵少/未成型）：保持旧逻辑——祭司可主动转化保命
    //      （远程兵优先 → 激进/保守最近敌人），不依赖己方兵先接战
    //    · 8000 帧后（临近第二波，战车弓兵登场）：新逻辑——先等己方兵攻击到目标再转化
    //      敌方 AI：敌人优先反击"最早攻击它的单位"（FindThreatToArmy 按首次攻击帧排序）。
    //      己方兵先打中 → 目标反击兵，不理会祭司 → 转化施法（2~6秒）安全走完；
    //      若祭司先转化 → 祭司成"最早攻击者" → 目标转火祭司（战车弓兵对祭司还有+7特攻）。
    bool aggressive = (info.civilizationStage >= CIVILIZATION_BRONZEAGE
                       || info.GameFrame > FRAME_WAVE1 + 3000);
    int target = -1;

    if (info.GameFrame < 8000) {
        // ===== 旧逻辑（8000帧前）：主动转化保命 =====
        // ① 优先远程兵（选最近的）
        double bestR = 1e18;
        for (const tagArmy& e : info.enemy_armies) {
            if (e.Blood <= 0) continue;
            if (e.Sort != AT_BOWMAN && e.Sort != AT_CHARIOT_ARCHER
                && e.Sort != AT_COMPOSITE_BOWMAN && e.Sort != AT_SLINGER) continue;
            double d = calDistance(priest->DR, priest->UR, e.DR, e.UR);
            if (d < bestR) { bestR = d; target = e.SN; }
        }
        if (target < 0 && aggressive) {
            // 激进：无远程兵 → 最近敌人
            double best = 1e18;
            for (const tagArmy& e : info.enemy_armies) {
                if (e.Blood <= 0) continue;
                double d = calDistance(priest->DR, priest->UR, e.DR, e.UR);
                if (d < best) { best = d; target = e.SN; }
            }
        } else if (target < 0) {
            // 保守：无远程兵 → 塔射程内最近敌人
            double bestD = 1e18;
            for (const tagArmy& e : info.enemy_armies) {
                if (e.Blood <= 0) continue;
                bool inTowerRange = false;
                for (const tagBuilding& b : info.buildings) {
                    if (b.Type != BUILDING_ARROWTOWER || b.Percent < 100) continue;
                    double d = calDistance(e.DR, e.UR,
                                           (double)b.BlockDR * BLOCKSIDELENGTH, (double)b.BlockUR * BLOCKSIDELENGTH);
                    if (d <= DIS_ARROWTOWER * BLOCKSIDELENGTH) { inTowerRange = true; break; }
                }
                if (inTowerRange) {
                    double d = calDistance(priest->DR, priest->UR, e.DR, e.UR);
                    if (d < bestD) { bestD = d; target = e.SN; }
                }
            }
        }
    } else {
        // ===== 新逻辑（8000帧后）：先让火力（兵/箭塔）锁到目标，祭司再转化 =====
        // 收集"正被己方火力攻击"的敌人：
        //   ① 己方兵 WorkObjectSN 指向的敌人
        //   ② 箭塔锁定目标（b.Project）——箭塔攻击同样拉仇恨（敌人转火打塔，不理会祭司）
        std::set<int> attackedByUs;
        for (const tagArmy& a : info.armies) {
            if (a.Sort == AT_PRIEST || a.Sort == AT_SCOUT) continue;   // 祭司/侦察骑兵不算"兵"
            if (a.WorkObjectSN <= 0) continue;
            for (const tagArmy& e : info.enemy_armies)
                if (e.SN == a.WorkObjectSN) { attackedByUs.insert(e.SN); break; }
        }
        for (const tagBuilding& b : info.buildings) {
            if (b.Type != BUILDING_ARROWTOWER || b.Percent < 100) continue;
            if (b.Project <= 0) continue;                              // 塔当前没锁定目标
            for (const tagArmy& e : info.enemy_armies)
                if (e.SN == b.Project) { attackedByUs.insert(e.SN); break; }
        }
        // ① 正被己方兵攻击的远程兵（选最近的）——安全转化（目标仇恨在兵身上）
        if (!attackedByUs.empty()) {
            double bestR = 1e18;
            for (const tagArmy& e : info.enemy_armies) {
                if (e.Blood <= 0) continue;
                if (!attackedByUs.count(e.SN)) continue;
                if (e.Sort != AT_BOWMAN && e.Sort != AT_CHARIOT_ARCHER
                    && e.Sort != AT_COMPOSITE_BOWMAN && e.Sort != AT_SLINGER) continue;
                double d = calDistance(priest->DR, priest->UR, e.DR, e.UR);
                if (d > CONVERT_MAX_DIST * BLOCKSIDELENGTH) continue;   // ★太远不追（否则等于出塔迎击）
                if (d < bestR) { bestR = d; target = e.SN; }
            }
            // ② 无被攻击的远程兵 → 被攻击的最近敌人
            if (target < 0) {
                double bestD = 1e18;
                for (const tagArmy& e : info.enemy_armies) {
                    if (e.Blood <= 0) continue;
                    if (!attackedByUs.count(e.SN)) continue;
                    if (!aggressive) {
                        // 保守：只在塔射程内的才转（祭司贴塔更安全）
                        bool inTowerRange = false;
                        for (const tagBuilding& b : info.buildings) {
                            if (b.Type != BUILDING_ARROWTOWER || b.Percent < 100) continue;
                            double d = calDistance(e.DR, e.UR,
                                                   (double)b.BlockDR * BLOCKSIDELENGTH, (double)b.BlockUR * BLOCKSIDELENGTH);
                            if (d <= DIS_ARROWTOWER * BLOCKSIDELENGTH) { inTowerRange = true; break; }
                        }
                        if (!inTowerRange) continue;
                    }
                    double d = calDistance(priest->DR, priest->UR, e.DR, e.UR);
                    if (d > CONVERT_MAX_DIST * BLOCKSIDELENGTH) continue;   // ★太远不追
                    if (d < bestD) { bestD = d; target = e.SN; }
                }
            }
        }
        // ③ attackedByUs 为空 → target 保持 -1：本帧不转化（等兵先接战建立仇恨）
    }

    // ===== 【修复·战车弓兵兜底转化】=====
    //   第二波窗口内，若上面没选出转化目标（我方兵/箭塔都还没锁定敌人），
    //   而拉怪又已经失败进过冷却 → 直接把射程内(DIS_PRIEST=12)的战车弓兵当目标。
    //   理由：它对祭司 +7 特攻、血只有 70，是最该被转化掉的目标；不转化就一直被它点。
    //   安全前提：祭司血 ≥60%（转化本身还要求 nearTower，见下面 2.4/2.5）。
    //   ★只在"拉怪失败过"之后才生效（m_lureCoolUntil>0），保证窗口前段行为与原来一致。
    if (target < 0 && wave2Defense && m_lureCoolUntil > 0
        && info.GameFrame >= m_lureCoolUntil
        && priest->Blood >= priest->MaxBlood * 3 / 5) {
        double bestCA = 1e18;
        for (const tagArmy& e : info.enemy_armies) {
            if (e.Blood <= 0) continue;
            if (e.Sort != AT_CHARIOT_ARCHER) continue;
            if (e.SN == m_lastConvertedSN) continue;
            double d = calDistance(priest->DR, priest->UR, e.DR, e.UR);
            if (d <= DIS_PRIEST * BLOCKSIDELENGTH && d < bestCA) { bestCA = d; target = e.SN; }
        }
    }

    // ===== G2 收尾兜底转化（第三波及以后的残敌）=====
    //   为什么需要：转化目标只从"我方兵/箭塔正在锁定的敌人"(attackedByUs) 里选，
    //   而投石车射程 10 > 箭塔 7 → 塔够不着它、我方兵又没出去打它 → attackedByUs 空
    //   → target 恒为 -1 → 只剩投石车时祭司永远不转化 ✗
    //   现在：我方没人锁定敌人 + 场上敌人已经不多(≤6) + 过了第二波 + 祭司血 ≥60%
    //   → 直接选"射程内(DIS_PRIEST=12)最近的敌人"，**投石车优先**（文档："投石车…可以
    //   最后处理，打掉或者转来自己用"）。
    //   ★前期不动：要求 frame > FRAME_WAVE2，第一波及之前行为完全不变。
    if (target < 0 && info.GameFrame > FRAME_WAVE2
        && (int)info.enemy_armies.size() <= 6
        && priest->ConvertCooldown <= 0
        && priest->Blood >= priest->MaxBlood * 3 / 5) {
        double bestD = 1e18;
        for (const tagArmy& e : info.enemy_armies) {
            if (e.Blood <= 0) continue;
            if (e.SN == m_lastConvertedSN) continue;
            double d = calDistance(priest->DR, priest->UR, e.DR, e.UR);
            if (d > CONVERT_MAX_DIST * BLOCKSIDELENGTH) continue;   // ★12→10，留余量
            if (e.Sort == AT_STONE_THROWER) {          // 投石车优先，直接选它
                target = e.SN;
                bestD = -1;
                break;
            }
            if (bestD >= 0 && d < bestD) { bestD = d; target = e.SN; }
        }
    }

    // ===== 【修复·D】拉怪优先于"主动转化" =====
    //   第二波窗口内，只要还有**第二个**战车弓兵活着，就先跑位把它引进塔射程，
    //   不要跑出去转化别的兵（实测"主动出击转化方阵兵"→ 出塔被近战围殴 → 死在回塔路上）。
    //   ★ 找"第二个"时必须双重排除：
    //     · m_convertTarget    = 正在下令转化的那个
    //     · m_lastConvertedSN  = 刚转化成功的那个（已变友军，快照可能残留）
    //   不排除就会把它当敌人 → 撤退方向算反 → 正好迎着第二个跑。
    bool needLure = false;
    const tagArmy* lureCA = nullptr;
    {
        // 【修复·第三波不拉怪】G1：
        //   ① 窗口原来到 FRAME_WAVE3(21000) 就失效 → 第三波全程不拉，改成"第二波前
        //      3000 帧之后一直有效"；
        //   ② 候选原来只认战车弓兵 → 第三波的复合弓/弓兵打祭司时 needLure 恒 false，
        //      改成"先战车弓兵（对祭司 +7 特攻最危险）、再复合弓/弓兵/投石兵"。
        const bool wave2LureWindow = (info.GameFrame > FRAME_WAVE2 - 3000);
        if (wave2LureWindow) {
            double lureD = 1e18;
            for (const tagArmy& e : info.enemy_armies) {          // 第一优先：战车弓兵
                if (e.Blood <= 0 || e.Sort != AT_CHARIOT_ARCHER) continue;
                if (e.SN == m_convertTarget) continue;      // 正在转化/刚下令的那个
                if (e.SN == m_lastConvertedSN) continue;    // 已经转化成功的那个（已是友军）
                double d = calDistance(priest->DR, priest->UR, e.DR, e.UR);
                if (d < lureD) { lureD = d; lureCA = &e; }
            }
            if (lureCA == nullptr) {                              // 其次：其它远程兵
                for (const tagArmy& e : info.enemy_armies) {
                    if (e.Blood <= 0) continue;
                    if (e.Sort != AT_COMPOSITE_BOWMAN && e.Sort != AT_BOWMAN
                        && e.Sort != AT_SLINGER) continue;
                    if (e.SN == m_convertTarget) continue;
                    if (e.SN == m_lastConvertedSN) continue;
                    double d = calDistance(priest->DR, priest->UR, e.DR, e.UR);
                    if (d < lureD) { lureD = d; lureCA = &e; }
                }
            }
            if (lureCA != nullptr
                && (lureD <= 14.0 * BLOCKSIDELENGTH || lureCA->WorkObjectSN == priestSN)) {
                needLure = true;
            }
        }
    }
    // 【修复·拉怪没有出口】needLure 原来只要"场上还有战车弓兵在 14 格内"就永远为真
    //   → 第 3 步主动转化被 `!needLure` 永久挡住（既不转化）＋ 第 3.5 步每帧重下令（来回打转）。
    //   现在给拉怪一个时限：超时没成事 → 进冷却，冷却期内强制不拉怪 → 转化就能跑起来。
    if (needLure) {
        if (m_lureStartFrame < 0) m_lureStartFrame = info.GameFrame;
        if (info.GameFrame - m_lureStartFrame > LURE_TIMEOUT_FRAMES) {
            m_lureCoolUntil = info.GameFrame + LURE_COOLDOWN_FRAMES;
            m_lureStartFrame = -1;
        }
    } else {
        m_lureStartFrame = -1;
    }
    if (info.GameFrame < m_lureCoolUntil) needLure = false;   // 冷却期内不拉怪

    // 2.4) 检查祭司是否在箭塔保护范围内（距最近塔 <= 6 格）——转化必须在塔下进行
    bool nearTower = false;
    for (const tagBuilding& b : info.buildings) {
        if (b.Type != BUILDING_ARROWTOWER || b.Percent < 100) continue;
        double d = calDistance(priest->DR, priest->UR,
                               (double)b.BlockDR * BLOCKSIDELENGTH, (double)b.BlockUR * BLOCKSIDELENGTH);
        if (d <= 6.0 * BLOCKSIDELENGTH) { nearTower = true; break; }
    }

    // 2.5) 转化节流：120 帧内只下一次转化令（无论目标是否变化）
    //      （之前"同目标120帧"有漏洞：目标一变（列表打乱/距离抖动）就绕过节流，
    //        导致每帧切换目标、转化永不完成）
    bool needConvertOrder = false;
    if (target >= 0 && priest->ConvertCooldown <= 0 && nearTower) {
        // 【修复】180 帧（7.2 秒）> 引擎最大施法 150 帧（6 秒）：保证不会在中途重下令打断转化
        bool tooSoon = (m_convertStartFrame >= 0 && info.GameFrame - m_convertStartFrame < 180);
        needConvertOrder = !tooSoon;   // 180 帧内不再下令，让本次转化走完
    }

    // 3) 有转化目标且节流通过 → 主动转化（敌人打别人时也转化，不等敌人打自己）
    if (needConvertOrder && !needLure) {   // 【修复·D】有"第二个战车弓兵"在场 → 先拉怪，不主动转化
        HumanAction(priestSN, target);
        m_issued.insert(priestSN);
        m_convertTarget = target;
        m_convertStartFrame = info.GameFrame;
        return;
    }

    // ===== 3.5) 【第二波专属·祭司拉怪跑位】把剩下的战车弓兵引进箭塔射程 =====
    //   战术：先转化掉 1 个战车弓（上面 1.6 已做）→ 立刻背对它往箭塔方向跑
    //   （getPriestHome = 离敌人来袭方向最远的塔）→ 战车追过来就落进箭塔射程(7格)
    //   → 箭塔用**原有索敌逻辑**锁定它；敌人一旦被锁，反击目标就粘在塔身上，
    //     只有塔死亡才解锁 → 它不再打祭司。
    //   触发：第二波窗口 + 没在转化 + 未濒死 + 场上有存活敌对战车弓兵（≤14格 或 正锁定祭司）
    //   位置：放在"主动转化"之后 → 有目标可转化时先转化，冷却期/无目标时才去拉怪。
    {
        // 【修复·C′】撤退终点不再是"塔中心"，而是"塔背面"（priestRetreatPoint）：
        //   站到塔背对追兵的一侧，追兵要打到祭司就必须绕过去 → 距塔 7-1.5 ≈ 5.5 格
        //   < 箭塔射程 7 → 箭塔锁得到它 → 仇恨从祭司转到塔上。
        // 【修复·打转】撤退点改成锁存点（priestRetreatLatched）→ 不再每帧跟着追兵变；
        // 【修复·不自保】加 !beingHit：正在挨打时让位给上面的转化逻辑（先保命/先转化）。
        // 【C 方案·波3不游走】波 3（>=FRAME_WAVE3）起不再为"拉怪"移动 ✗
        //   （小兵现在会分散去拉不同远程兵的仇恨 ✓，不需要祭司自己跑位）
        //   注意：只加闸门 —— 转化逻辑（1.6 自卫 / 主动 2-3 / G2 兜底）完全没动 ✓
        if (info.GameFrame < FRAME_WAVE3
            && needLure && lureCA != nullptr && !inConversion && !criticalBlood && !beingHit) {
            int hx, hy;
            getPriestHome(info, hx, hy);
            if (hx >= 0) {
                double gx, gy;
                priestRetreatLatched(0, lureCA->DR, lureCA->UR, hx, hy,
                                     priest->DR, priest->UR, info.GameFrame, gx, gy);
                if (movePriest(priestSN, priest->DR, priest->UR, gx, gy, info.GameFrame))
                    return;             // 已下令撤向"塔背面"（锁存点）→ 本帧结束
            }
        }
    }

    // 4) 无可转化目标：检测威胁（非转化目标的敌人）
    double threatDist = 10.0 * BLOCKSIDELENGTH;
    double nearest = 1e18;
    const tagArmy* threat = nullptr;
    for (const tagArmy& e : info.enemy_armies) {
        if (e.SN == target) continue;                 // 排除转化目标
        double d = calDistance(priest->DR, priest->UR, e.DR, e.UR);
        if (d < nearest) { nearest = d; threat = &e; }
    }

    // 5) 有其他威胁（非转化目标）→ 贴塔走位
    //    目标 = getPriestHome（敌人反侧最远的塔，固定值）——与"被攻击走位/回塔下"目标统一，
    //    防止"最近塔"与"敌人反侧塔"两个不同目标交替触发 → 祭司两点来回横跳
    // 【修复】转化进行中不许走位打断（HumanMove 会 suspendRelation → 转化作废重来）
    // 【C 方案·波3不游走】波 3 起不再为"躲威胁"移动 ✗（原来也是移到塔背面 → 追兵一动就换边 ✗）
    //   不 return → 流程继续到第 6 步"回塔待命"，所以他照样会自己走回塔下 ✓
    if (info.GameFrame < FRAME_WAVE3
        && !inConversion && threat != nullptr && nearest < threatDist) {
        int hx, hy;
        getPriestHome(info, hx, hy);
        if (hx >= 0) {
            // 【修复·C′】撤退终点同样用"塔背面"，否则下一步的"回塔下"会把他拉回塔心，
            //   追兵又停在射程外 → 拉怪再次失效。
            double gx, gy;
            // 【修复·打转】同 3.5：这里也用锁存点，否则"威胁撤退"同样会每帧换目标 → 抽搐
            priestRetreatLatched(1, threat->DR, threat->UR, hx, hy,
                                 priest->DR, priest->UR, info.GameFrame, gx, gy);
            movePriest(priestSN, priest->DR, priest->UR, gx, gy, info.GameFrame);
        }
        return;
    }

    // 6) 无威胁且无转化目标：不在塔下 → 回塔下待命（节流下令）
    // 【修复】同上：转化进行中不回塔待命，先把这次转化走完
    if (!inConversion && !nearTower && info.GameFrame > FRAME_WAVE1 - 2000) {
        int hx, hy;
        getPriestHome(info, hx, hy);
        if (hx >= 0) {
            movePriest(priestSN, priest->DR, priest->UR, (double)hx * BLOCKSIDELENGTH, (double)hy * BLOCKSIDELENGTH, info.GameFrame);
        }
    }
}

// ============================================================
// 【反击前置·阶段S】侦察敌方主营 / 武器工程厂
//   为什么必须探路：取胜条件需要 BUILDING_SIEGE 的 SN，而 Core.cpp:621 中
//   i==0（我们）的 enemy_buildings 只收 explored==1 || visible==1 的建筑；
//   GameWidget.cpp:181 的注释确认 explored 一旦置 1 永不回退
//   → 侦察兵只要"看到一次"就够，不必在敌营边上保持视野。
//
//   ★隔离原则（本模块不干扰任何原有逻辑）：
//     1) 只动用 1 个"耗材"兵；绝不碰祭司、不碰农民、不碰建筑；
//     2) 帧 < SCOUT_START_FRAME 时整段 return —— 前期一行都不执行；
//     3) 本模块在 processData 里排在 defense() **之前**，并把侦察兵 SN 放进 m_issued
//        → defense() 内 `if (m_issued.count(a.SN)) continue;`（:2083 / :2151）自动跳过它
//        → 不改 defense() 一行；
//     4) handlePriest / scoutWithPriest / scoutWithScout 同样尊重 m_issued → 也不会抢；
//     5) 任务结束（成功或放弃）后不再插入 m_issued → 该兵自动回归正常军队管理。
// ============================================================
static const int SCOUT_START_FRAME = 15000;   // 【用户要求】第二波(13500)之后才开始考虑探路
                                              //   （真正出发还要满足下面的"场上没有可见敌人"✓）   // 阶段S 启动帧（原来 18000，太晚：常常还没到就打完/判负）
static const int SCOUT_MIN_ARMY    = 1;       // 至少这么多兵（不含祭司/侦察骑兵）才派人（6→4→3，兵少也要能探路）
static const int SCOUT_WP_REACH    = 4;       // 距路点这么近算到达（格）
static const int SCOUT_WP_TIMEOUT  = 600;     // 同一路点耗这么久就跳下一个（防卡死）
static const int SCOUT_GIVEUP_LOST = 3;       // 耗材死这么多就放弃探路
static const int SCOUT_GIVEUP_EXPL = 95;      // 已探明比例(%)达到就放弃
static const int SCOUT_DONE_FRAME  = 99999;   // 到这一帧无论结果都收工（22500→26000，窗口拉长）

static int m_scoutWPI = 0;                     // 当前路点序号
static int m_scoutLastX = -1, m_scoutLastY = -1;    // 【用户要求】侦察兵最近一次位置（逐帧更新 ✓）
static int m_scoutDeathX = -1, m_scoutDeathY = -1;  // 【用户要求】侦察兵**首次阵亡**地点（反攻集结点基准 ✓）
// 【修复·换人不换路】"第几趟"必须跨侦察兵持久 ✗ —— 原来派人时 m_scoutWPI=0，
//   导致 idx 永远从 0 开始、leg 永远是 0 → 每一任探路兵都走同一个角 ✗
static int m_scoutLeg = 0;                          // 当前趟次（0,1,2,3…）
static const int SCOUT_WP_PER_LEG = 8;              // 每趟 8 个路点（与 scoutWaypoint 里 idx/8 一致 ✓）
static int m_scoutWPX = -1, m_scoutWPY = -1;   // 当前路点（块坐标）
static int m_scoutWpFrame = -1;                // 进入当前路点的帧（超时用）
static int m_scoutOrderFrame = -9999;          // 上次下令帧
static int m_scoutLost = 0;                    // 耗材损失数
static int m_scoutExplChk = -9999;             // 上次统计探明率的帧
static int m_scoutExplPct = 0;                 // 已探明比例(%)
static int m_scoutBldSeen = 0;                 // 见过的敌方建筑数（峰值）
static int m_scoutDbgFrame = -9999;            // 【诊断】上次打 [SCOUT] 的帧
static int m_scoutGoFrame = -1;                // 【诊断】第一次真正派出侦察兵的帧
static int m_scoutEndFrame = -1;               // 【诊断】收工帧
static int m_scoutDoneWhy = 0;                 // 【诊断】收工原因 1=看到厂 2=敌建筑≥2 3=帧 4=死3个 5=探明率
static int m_siegeSN = -1, m_siegeX = -1, m_siegeY = -1;   // ★取胜目标：敌方武器工程厂
static int m_enemyBaseX = -1, m_enemyBaseY = -1;           // 敌方建筑群中心（后续集结用）

static double scoutBD(int b) { return ((double)b + 0.5) * BLOCKSIDELENGTH; }

static double scoutDist(int x1, int y1, int x2, int y2)
{
    const double dx = (double)(x1 - x2), dy = (double)(y1 - y2);
    return sqrt(dx * dx + dy * dy);
}

// 战斗单位数（不含祭司 / 侦察骑兵）—— 与 defense() 里 armyCnt 口径一致
static int scoutArmyCount(const tagInfo& info)
{
    int n = 0;
    for (const tagArmy& a : info.armies)
        if (a.Sort != AT_PRIEST && a.Sort != AT_SCOUT) ++n;
    return n;
}

// 统计各类"可用于侦察"的兵（不含祭司/侦察骑兵）
static void scoutCounts(const tagInfo& info, int& nBow, int& nSword, int& nComp, int& nOther)
{
    nBow = nSword = nComp = nOther = 0;
    for (const tagArmy& a : info.armies) {
        if (a.Sort == AT_PRIEST || a.Sort == AT_SCOUT) continue;
        if (a.Sort == AT_BOWMAN) ++nBow;
        else if (a.Sort == AT_BROADSWORDSMAN) ++nSword;
        else if (a.Sort == AT_COMPOSITE_BOWMAN) ++nComp;
        else ++nOther;
    }
}

// 选探路耗材：【用户要求】从所有"空闲的战斗单位"里**随机挑一个**
//   · 排除祭司（他一死立刻判负）、侦察骑兵
//   · 不看兵种、不看数量 —— 兵多的时候随便哪个都行
//   · 死亡后下一帧会再随机派一个（见 scoutPhaseS 第 5/6 步）
static int scoutPick(const tagInfo& info)
{
    int cand[64];                 // 首选：空闲的兵
    int n = 0;
    int cand2[64];                // 次选：只是在"走路"的兵（没在采集/攻击）
    int n2 = 0;
    for (const tagArmy& a : info.armies) {
        if (a.Sort == AT_PRIEST || a.Sort == AT_SCOUT) continue;   // 祭司一死立刻判负，绝不派
        if (m_issued.count(a.SN)) continue;
        if (a.NowState == HUMAN_STATE_IDLE) {
            if (n < 64) cand[n++] = a.SN;
        } else if (a.NowState == HUMAN_STATE_WALKING && a.WorkObjectSN <= 0) {
            // 【修复·派不出去】纯移动（没有工作目标）→ 多半是 defense 的"回塔集结"下的移动令，
            //   本来就只是往家走，征用去探路没有副作用 ✓（在采集/攻击的兵不会被选）
            if (n2 < 64) cand2[n2++] = a.SN;
        }
    }
    if (n > 0) return cand[Rand.nextRaw() % n];        // 优先空闲的
    if (n2 > 0) return cand2[Rand.nextRaw() % n2];     // 其次纯走路的
    return -1;
}

// 路点：自家基地 → 敌方对角（若已见过敌人则用 m_enemyDirX/Y 修正）
//   前 4 个点做成之字形推进，之后在敌方角落附近横向往复扫
static void scoutWaypoint(int idx, int& wx, int& wy)
{
    const int mL = MAP_L, mU = MAP_U;
    int cx = m_centerX, cy = m_centerY;
    if (cx < 0) cx = mL / 2;
    if (cy < 0) cy = mU / 2;
    int ex = (cx < mL / 2) ? (mL - 12) : 12;      // 敌人一般在自家对角
    int ey = (cy < mU / 2) ? (mU - 12) : 12;
    // 【用户要求·探路一趟走完没发现敌人就换路】每 8 个路点算"一趟"（leg = idx/8）：
    //   一趟 = 3 个之字推进点 + 到角 + 4 点绕角扫 → 走完还没看到敌人 → 换下一个角 ✓
    //   原来 default 分支只在**同一个角**往复扫（idx%4）→ 敌人不在那儿就永远找不到 ✗
    const int leg = idx / 8;                      // 第几趟（0,1,2,3,…）
    const int li  = idx % 8;                      // 本趟内的序号
    {
        const int farX = (cx < mL / 2) ? (mL - 12) : 12;
        const int farY = (cy < mU / 2) ? (mU - 12) : 12;
        const int otherX = (farX == (mL - 12)) ? 12 : (mL - 12);
        const int otherY = (farY == (mU - 12)) ? 12 : (mU - 12);
        switch (leg % 4) {                        // ★四角轮换：一趟换一个角
        case 0:  ex = farX;   ey = farY;   break; // 对角（原来唯一的方向）
        case 1:  ex = farX;   ey = otherY; break;
        case 2:  ex = otherX; ey = farY;   break;
        default: ex = otherX; ey = otherY; break;
        }
    }
    if (m_enemyDirX > 0) ex = mL - 12;            // 已见过敌人 → 按实际来袭方向修正
    else if (m_enemyDirX < 0) ex = 12;
    if (m_enemyDirY > 0) ey = mU - 12;
    else if (m_enemyDirY < 0) ey = 12;
    // ★但若"这一趟"已经走完（leg 递增）说明上一趟没找到 → 允许轮换覆盖上面的方向修正 ✓
    if (leg > 0) {
        const int farX2 = (cx < mL / 2) ? (mL - 12) : 12;
        const int farY2 = (cy < mU / 2) ? (mU - 12) : 12;
        const int otherX2 = (farX2 == (mL - 12)) ? 12 : (mL - 12);
        const int otherY2 = (farY2 == (mU - 12)) ? 12 : (mU - 12);
        switch (leg % 4) {
        case 1:  ex = farX2;   ey = otherY2; break;
        case 2:  ex = otherX2; ey = farY2;   break;
        case 3:  ex = otherX2; ey = otherY2; break;
        default: break;
        }
    }

    switch (li) {
    case 0:  wx = cx + (ex - cx) / 3;      wy = cy + (ey - cy) / 3;      break;
    case 1:  wx = cx + (ex - cx) * 2 / 3;  wy = cy + (ey - cy) / 3;      break;
    case 2:  wx = cx + (ex - cx) * 2 / 3;  wy = cy + (ey - cy) * 2 / 3;  break;
    case 3:  wx = ex;                      wy = ey;                      break;
    default: {
        const int span = 14;
        const int k = li % 4;
        wx = ex + (((ex < mL / 2) ? span : -span) * ((k < 2) ? 1 : 0));
        wy = ey + ((k % 2) ? span : -span);
        break;
    }
    }
    if (wx < 2) wx = 2;
    if (wx > mL - 3) wx = mL - 3;
    if (wy < 2) wy = 2;
    if (wy > mU - 3) wy = mU - 3;
}

static void scoutPhaseS(UsrAI* self, const tagInfo& info)
{
    const int f = info.GameFrame;

    // ---------- 0) 【诊断】从 15000 帧起每 600 帧（24 秒）打一行到 stdout：
    //   本地 Qt Creator 的"应用程序输出"面板、以及 OJ 的标准输出日志都能看到，
    //   直接说明"派没派 / 为什么没派"。日志太多时把这几行删掉即可（不影响逻辑）。
    if (f >= 15000 && f - m_scoutDbgFrame >= 600) {
        m_scoutDbgFrame = f;
        int nB = 0, nS = 0, nC = 0, nO = 0;
        scoutCounts(info, nB, nS, nC, nO);
        int nIdle = 0;                        // 当前"空闲可派"的战斗单位数
        for (const tagArmy& a : info.armies) {
            if (a.Sort == AT_PRIEST || a.Sort == AT_SCOUT) continue;
            if (a.NowState != HUMAN_STATE_IDLE) continue;
            ++nIdle;
        }
        std::cout << "[SCOUT] f=" << f
                  << " idle=" << nIdle
                  << " done=" << m_scoutDone
                  << " unit=" << m_scoutUnitSN
                  << " army=" << scoutArmyCount(info) << "/" << SCOUT_MIN_ARMY
                  << " bow=" << nB << " sword=" << nS << " comp=" << nC << " other=" << nO
                  << " lost=" << m_scoutLost
                  << " bldSeen=" << m_scoutBldSeen
                  << " siegeSN=" << m_siegeSN
                  << " expl=" << m_scoutExplPct << "%"
                  << " wp=" << m_scoutWPI << "(" << m_scoutWPX << "," << m_scoutWPY << ")"
                  << std::endl;
        // 【面板也打一份】stdout 只有 Qt Creator 的"应用程序输出"/OJ 日志能看到，
        //   游戏内右侧面板走的是 call_debugText → 这里补一条，本地调试更直观
        self->DebugText(QString("[SCOUT]f%1 done%2 unit%3 army%4/%5 idle%6 bld%7 siege%8 expl%9")
                        .arg(f).arg(m_scoutDone).arg(m_scoutUnitSN)
                        .arg(scoutArmyCount(info)).arg(SCOUT_MIN_ARMY).arg(nIdle)
                        .arg(m_scoutBldSeen).arg(m_siegeSN).arg(m_scoutExplPct));
    }

    // ---------- 1) 观测：只要 info 里出现敌方建筑就登记（不依赖侦察兵）----------
    int bldCnt = 0, sumX = 0, sumY = 0;
    for (const tagBuilding& b : info.enemy_buildings) {
        ++bldCnt;
        sumX += b.BlockDR;
        sumY += b.BlockUR;
        if (b.Type == BUILDING_SIEGE && m_siegeSN < 0) {
            m_siegeSN = b.SN;                 // 条件A：看到武器工程厂 → 目标锁定
            if (m_scoutDoneWhy == 0) m_scoutDoneWhy = 1;
            m_siegeX = b.BlockDR;
            m_siegeY = b.BlockUR;
            m_scoutDone = 1;                  //           → 立刻收工
        }
    }
    if (bldCnt > 0) {                         // 敌方建筑群中心（列表每帧被打乱，取平均才稳定）
        m_enemyBaseX = sumX / bldCnt;
        m_enemyBaseY = sumY / bldCnt;
    }
    if (bldCnt > m_scoutBldSeen) m_scoutBldSeen = bldCnt;
    if (m_scoutBldSeen >= 4) {                // round2#7b 阈值 2→4（看到两座房子就收工太早 ✗）
        m_scoutDone = 1;
        if (m_scoutDoneWhy == 0) m_scoutDoneWhy = 2;
    }

    // ---------- 2) 放弃条件（条件C）----------
    if (!m_scoutDone) {
        if (f >= SCOUT_DONE_FRAME) {
            m_scoutDone = 1;                  // 到集结线了，不能再等
            if (m_scoutDoneWhy == 0) m_scoutDoneWhy = 3;
        } else if (m_scoutLost >= SCOUT_GIVEUP_LOST) {
            m_scoutDone = 1;                  // 耗材死太多 → 改由军队推进时点亮
            if (m_scoutDoneWhy == 0) m_scoutDoneWhy = 4;
        } else if (f - m_scoutExplChk >= 300) {
            m_scoutExplChk = f;
            int seen = 0;
            for (int i = 0; i < 100; ++i)     // m_map: -2 = 未探索
                for (int j = 0; j < 100; ++j)
                    if (m_map[i][j] != -2) ++seen;
            m_scoutExplPct = seen / 100;      // 10000 格 → 百分比
            if (m_scoutExplPct >= SCOUT_GIVEUP_EXPL) { m_scoutDone = 1; if (m_scoutDoneWhy == 0) m_scoutDoneWhy = 5; }
        }
    }

    // 【诊断】收工只打一次：告诉用户"为什么不再探了"
    if (m_scoutDone && m_scoutEndFrame < 0) {
        m_scoutEndFrame = f;
        std::cout << "[SCOUT_END] f=" << f << " why=" << m_scoutDoneWhy
                  << " (1=看到厂 2=敌建筑>=2 3=帧 4=死3个 5=探明率)"
                  << " siegeSN=" << m_siegeSN << " bldSeen=" << m_scoutBldSeen
                  << " lost=" << m_scoutLost << " expl=" << m_scoutExplPct << std::endl;
        self->DebugText(QString("[SCOUT_END]f%1 why%2 siege%3 bld%4 lost%5 expl%6")
                        .arg(f).arg(m_scoutDoneWhy).arg(m_siegeSN)
                        .arg(m_scoutBldSeen).arg(m_scoutLost).arg(m_scoutExplPct));
    }
    // 任务已结束 → 本模块不再下令；该兵不再进 m_issued → 自动回归 defense 管理
    if (m_scoutDone) {
        // ★round2#1 必须清零：否则 attackPhase 的 atkArmyCount 把他算进 army ✓
        //   但下令循环又排除他 ✗ → inPlace >= army / atFront >= army 永远为假
        //   → 集结只能等超时、**前压点永远不动**（反攻卡到 34000 帧）✗✗
        m_scoutUnitSN = -1;
        return;
    }

    // ---------- 3) 门没开 → 完全不动（前期一行不执行）----------
    if (f < SCOUT_START_FRAME) return;
    // round2#7 【修复·探路兵永远派不出去】原来"场上一个可见敌人都不能有"过于苛刻 ✗
    //   → 改成"敌人不在基地附近(30 格)就允许出发"✓（远方的敌方单位/农民不再挡住探路 ✓）
    {
        const int hcx = (m_centerX >= 0) ? m_centerX : MAP_L / 2;
        const int hcy = (m_centerY >= 0) ? m_centerY : MAP_U / 2;
        const double NEARHOME = 30.0;
        bool nearHome = false;
        for (const tagArmy& e : info.enemy_armies)
            if (scoutDist(e.BlockDR, e.BlockUR, hcx, hcy) <= NEARHOME) { nearHome = true; break; }
        if (!nearHome)
            for (const tagFarmer& e : info.enemy_farmers)
                if (scoutDist(e.BlockDR, e.BlockUR, hcx, hcy) <= NEARHOME) { nearHome = true; break; }
        if (nearHome) return;                 // 敌人已贴到家门口 → 不派探路兵
    }
    if (scoutArmyCount(info) < SCOUT_MIN_ARMY) return;

    // ---------- 4) 侦察兵存活检查 ----------
    const tagArmy* me = nullptr;
    if (m_scoutUnitSN >= 0) {
        for (const tagArmy& a : info.armies)
            if (a.SN == m_scoutUnitSN) { me = &a; break; }
        if (me != nullptr) {              // 【用户要求】逐帧记录位置 → 阵亡那一刻就是死亡地点 ✓
            m_scoutLastX = (int)(me->DR / BLOCKSIDELENGTH);
            m_scoutLastY = (int)(me->UR / BLOCKSIDELENGTH);
        }
        if (me == nullptr) {                  // 阵亡 → 记一次损失，下一帧换人
            ++m_scoutLeg;                     // 【修复·换人不换路】下一任换下一个角 ✓
            // 【用户要求·集结点】第一次阵亡 → 记下地点（之后不再改 ✓）
            if (m_scoutDeathX < 0 && m_scoutLastX >= 0) {
                m_scoutDeathX = m_scoutLastX;
                m_scoutDeathY = m_scoutLastY;
            }
            m_scoutUnitSN = -1;
            ++m_scoutLost;
            return;
        }
    }

    // ---------- 5) 派人 ----------
    if (m_scoutUnitSN < 0) {
        const int sn = scoutPick(info);
        if (sn < 0) return;                   // 暂时没有合适的耗材（等新兵）→ 本帧不派
        m_scoutUnitSN = sn;
        // 【修复·换人不换路】从**当前趟**的第一个路点出发（而不是永远从 0 号点 ✗）
        m_scoutWPI = m_scoutLeg * SCOUT_WP_PER_LEG;
        scoutWaypoint(m_scoutWPI, m_scoutWPX, m_scoutWPY);
        self->HumanMove(sn, scoutBD(m_scoutWPX), scoutBD(m_scoutWPY));
        m_issued.insert(sn);
        m_scoutOrderFrame = f;
        m_scoutWpFrame = f;
        // 【诊断】第一次真正派出去 → 立刻打一行（stdout + 面板），免得被刷屏淹没
        if (m_scoutGoFrame < 0) {
            m_scoutGoFrame = f;
            std::cout << "[SCOUT_GO] f=" << f << " scoutSN=" << sn
                      << " wp0=(" << m_scoutWPX << "," << m_scoutWPY << ")"
                      << " army=" << scoutArmyCount(info) << std::endl;
            self->DebugText(QString("[SCOUT_GO]f%1 sn%2 wp(%3,%4) army%5")
                            .arg(f).arg(sn).arg(m_scoutWPX).arg(m_scoutWPY)
                            .arg(scoutArmyCount(info)));
        }
        return;
    }

    // ---------- 6) 走路点 ----------
    const double d = scoutDist(me->BlockDR, me->BlockUR, m_scoutWPX, m_scoutWPY);
    const bool arrived = (me->NowState == HUMAN_STATE_IDLE && d <= (double)SCOUT_WP_REACH);
    const bool wpTimeout = (m_scoutWpFrame > 0 && f - m_scoutWpFrame >= SCOUT_WP_TIMEOUT);
    if (arrived || wpTimeout) {
        if (wpTimeout) m_scoutWPI += 2;       // 卡住了 → 跳过一格、换个方向
        else           ++m_scoutWPI;          // 正常到达 → 下一个点
        scoutWaypoint(m_scoutWPI, m_scoutWPX, m_scoutWPY);
        self->HumanMove(m_scoutUnitSN, scoutBD(m_scoutWPX), scoutBD(m_scoutWPY));
        m_issued.insert(m_scoutUnitSN);
        m_scoutOrderFrame = f;
        m_scoutWpFrame = f;
        return;
    }
    // 停下来了（被挡/被撞）→ 每 50 帧推一次，继续往路点走
    if (me->NowState == HUMAN_STATE_IDLE && f - m_scoutOrderFrame >= 50) {
        self->HumanMove(m_scoutUnitSN, scoutBD(m_scoutWPX), scoutBD(m_scoutWPY));
        m_issued.insert(m_scoutUnitSN);
        m_scoutOrderFrame = f;
    }
}

// ============================================================
// 【反攻阶段】集结 → 推进拉扯 → 交战 → 祭司冲锋转化武器工程厂
//   胜利条件（MainWidget::isWin）：player[0]->build 里出现
//     getNum()==BUILDING_SIEGE && isConverted() && !isDie()
//   → 只能靠祭司贴上去转化，**绝不能把厂打掉**（打掉就永远赢不了）。
//
//   为什么必须"区域集结"：引擎寻路查 map_Object[][].empty()，一格站了人就当不可达
//   → 所有人下一个点只有一个人能到位；必须用一个区域、每格一个兵（文档 44 行）。
//
//   隔离：触发线没到之前一行都不执行；只对"空闲"的兵下令；
//   调用点在 processData 最后 → 同帧最后一条令，引擎按对象去重时保留它，
//   所以**不需要改 defense()**（defense 只下令空闲的兵）。
// ============================================================
static const int ATK_EARLY_FRAME  = 21000;   // 提前线：第三波刚开始 + 兵很多 → 早点打出去
static const int ATK_EARLY_ARMY   = 24;
static const int ATK_MAIN_FRAME   = 22500;   // 主启动线（推荐）
static const int ATK_MAIN_ARMY    = 18;
static const int ATK_LAST_FRAME   = 38000;   // 兜底线（27000→38000 ≈25:20）：**主要条件仍是人口满** ✓
                                             //   原来 27000(18:00) 太早 —— 实测 19 分钟人口没满就冲出去了 ✗
static const int ATK_LAST_ARMY    = 15;      // 兜底线的兵力门槛 10→15（别拿 10 个兵去送 ✗）
static const int ATK_RALLY_BACK   = 4;       // 集结点 = 敌方目标往自家方向退几格
static const int ATK_RALLY_R      = 2;       // 集结区半径（2 → 5×5，每格一个兵）
static const int ATK_RALLY_HOME_PULL = 12;   // 【用户要求】集结点=阵亡点再朝大本营拉近几格（10→12，再靠家 2 格 ✓）
static const int ATK_RALLY_WAIT_MAX  = 1500; // 【用户要求】等齐了再上；最多等 60 秒（防一个兵卡住全队 ✗）
static const int ATK_FRONT_STEP   = 6;       // 推进时全体前压几格（再重新铺开）
static const int ATK_FRONT_GAP    = 60;      // 两次前压之间至少间隔多少帧
static const double ATK_RALLY_FRAC = 0.5;    // 集结点 = 自家基地→目标点 连线的这个比例处
                                             //   （0.5=中点；越小越靠家=越安全；原来等于"贴着敌营" ✗）
static const int ATK_NEAR_ENEMY   = 8;       // 兵身边多少格内有敌人 → 直接打它
static const int ATK_ASSAULT_NEAR = 6;       // 突击者多少格内有敌人 → 撤回初始位置
static const int ATK_PUSH_FRAME   = 34000;   // 到这一帧无论如何冲锋
static const int ATK_MAX_ORDER    = 8;       // 每帧最多下这么多条令（省引擎的指令配额）
static const int ATK_PRIEST_SAFE  = 10;      // 祭司距厂多少格内就贴上去转化
static const int ATK_ABORT_ARMY   = 6;       // 【修复·兵不够还硬冲】反攻中兵力低于这个数 → 撤销反攻 ✓
// 【策略文档·早集结早开战】文档(75行)："第二波防御一过就集结，集结 10 个左右复合弓就可以开战，
//   差不多是 11 分钟多点" → 主要触发线 = 兵力 ≥ 10 且 帧 ≥ 16500(11:00) ✓
static const int ATK_BOW_ARMY     = 10;      // 早开战兵力门槛（≈10 个复合弓 ✓）
static const int ATK_BOW_FRAME    = 16500;   // 早开战时间门槛（11:00 ✓ 第二波之后 ✓）
static const int ATK_CHARGE_MAXEN = 8;       // 【修复·被围还冲锋】敌人多于这个数 → 冲锋降级为阶段2走位 ✓
static const int ATK_FRONT_CLEAR  = 12;      // 【用户要求·一层层拉出来打】前方这么多格内有敌人 → 原地打完再推进 ✓

static int m_atkTargetX = -1, m_atkTargetY = -1;   // 进攻目标点
static int m_atkRallyX = -1, m_atkRallyY = -1;     // 集结点
static int m_atkFrontX = -1, m_atkFrontY = -1;     // 当前前压点
static int m_assaultSN = -1;                       // 突击者 SN
static double m_assaultPX = 0, m_assaultPY = 0;    // 突击者初始位置（细节坐标）
static int m_atkPhaseFrame = -9999;                // 进入当前阶段的帧
static int m_atkDbgFrame = -9999;                  // 诊断输出节流

static double atkBD(int b) { return ((double)b + 0.5) * BLOCKSIDELENGTH; }

static double atkDist(double x1, double y1, double x2, double y2)
{
    const double dx = x1 - x2, dy = y1 - y2;
    return sqrt(dx * dx + dy * dy);
}

// 战斗单位数（不含祭司/侦察骑兵）
static int atkArmyCount(const tagInfo& info)
{
    int n = 0;
    for (const tagArmy& a : info.armies)
        if (a.Sort != AT_PRIEST && a.Sort != AT_SCOUT) ++n;
    return n;
}

// 目标点：武器工程厂 > 敌方建筑群中心 > 地图对角（降级链，保证永远有地方可去）
// 集结点：目标点往自家（市中心）方向退 ATK_RALLY_BACK 格
static void atkPickPoints(const tagInfo& info)
{
    if (m_siegeSN >= 0 && m_siegeX >= 0) {
        m_atkTargetX = m_siegeX;
        m_atkTargetY = m_siegeY;
    } else if (m_enemyBaseX >= 0) {
        m_atkTargetX = m_enemyBaseX;
        m_atkTargetY = m_enemyBaseY;
    } else {
        const int mL = MAP_L, mU = MAP_U;
        const int cx = (m_centerX >= 0) ? m_centerX : mL / 2;
        const int cy = (m_centerY >= 0) ? m_centerY : mU / 2;
        m_atkTargetX = (cx < mL / 2) ? (mL - 10) : 10;
        m_atkTargetY = (cy < mU / 2) ? (mU - 10) : 10;
    }
    // 【修复·祭司往外跑】集结点原来是"目标点往自家退 4 格"= 贴着敌营 ✗ →
    //   反攻一触发就把祭司派到敌营门口（现象：他主动离开箭塔往外跑）。
    //   现在改成"自家基地 → 目标点"连线的 ATK_RALLY_FRAC(默认 0.5=中点) 处：真正的后方集结区 ✓
    const int hx = (m_centerX >= 0) ? m_centerX : MAP_L / 2;
    const int hy = (m_centerY >= 0) ? m_centerY : MAP_U / 2;
    m_atkRallyX = hx + (int)((double)(m_atkTargetX - hx) * ATK_RALLY_FRAC + 0.5);
    m_atkRallyY = hy + (int)((double)(m_atkTargetY - hy) * ATK_RALLY_FRAC + 0.5);
    m_atkFrontX = m_atkRallyX;
    m_atkFrontY = m_atkRallyY;
    const int mL2 = MAP_L, mU2 = MAP_U;
    if (m_atkRallyX < 1) m_atkRallyX = 1;
    if (m_atkRallyX > mL2 - 2) m_atkRallyX = mL2 - 2;
    if (m_atkRallyY < 1) m_atkRallyY = 1;
    if (m_atkRallyY > mU2 - 2) m_atkRallyY = mU2 - 2;
    m_atkFrontX = m_atkRallyX;
    m_atkFrontY = m_atkRallyY;
}

// 以 (bx,by) 为中心、半径 r 的方块里第 slot 个格子的坐标（每格一个兵，避免"站满不可达"）
static void atkSlotPos(int bx, int by, int r, int slot, int& sx, int& sy)
{
    const int side = 2 * r + 1;
    int k = slot % (side * side);
    if (k < 0) k += side * side;
    sx = bx + (k % side) - r;
    sy = by + (k / side) - r;
    if (sx < 1) sx = 1;
    if (sx > MAP_L - 2) sx = MAP_L - 2;
    if (sy < 1) sy = 1;
    if (sy > MAP_U - 2) sy = MAP_U - 2;
}

// 前压点：从集结点朝目标点推进 step 格（不越过目标点）
static void atkAdvanceFront(int step)
{
    double vx = (double)(m_atkTargetX - m_atkRallyX);
    double vy = (double)(m_atkTargetY - m_atkRallyY);
    double len = sqrt(vx * vx + vy * vy);
    if (len < 1e-6) return;
    m_atkFrontX = m_atkRallyX + (int)(vx / len * step + 0.5);
    m_atkFrontY = m_atkRallyY + (int)(vy / len * step + 0.5);
}

// 【修复·推不动】从"当前前压点"再朝目标推进 step 格（跳板式前进用）
//   原来的 atkAdvanceFront 永远从集结点算 → 只能推一次，推到一半就停住 ✗
static void atkFrontForward(int step)
{
    double vx = (double)(m_atkTargetX - m_atkFrontX);
    double vy = (double)(m_atkTargetY - m_atkFrontY);
    double len = sqrt(vx * vx + vy * vy);
    if (len < 1e-6) return;
    int nx = m_atkFrontX + (int)(vx / len * step + 0.5);
    int ny = m_atkFrontY + (int)(vy / len * step + 0.5);
    if (nx < 1) nx = 1;
    if (nx > MAP_L - 2) nx = MAP_L - 2;
    if (ny < 1) ny = 1;
    if (ny > MAP_U - 2) ny = MAP_U - 2;
    m_atkFrontX = nx;
    m_atkFrontY = ny;
}

static void attackPhase(UsrAI* self, const tagInfo& info)
{
    const int f = info.GameFrame;

    // 找祭司
    int priestSN = -1;
    const tagArmy* priest = nullptr;
    for (const tagArmy& a : info.armies)
        if (a.Sort == AT_PRIEST) { priestSN = a.SN; priest = &a; break; }

    const int army = atkArmyCount(info);

    // ---------- 0) 【修复·兵不够还硬冲】兵力过少 → 撤销反攻，退回防守 ✓ ----------
    //   否则新造出来的兵会被一个个送进冲锋里打死 ✗，家里也没人守 ✗（截图实证 army4 / enemy24 ✓）
    if (m_atkOn && army < ATK_ABORT_ARMY) {
        m_atkOn = 0;
        m_atkPhase = 0;
        m_assaultSN = -1;
        m_atkPhaseFrame = f;
        return;                             // 本帧不再下反攻令；下一帧 defense 接管、把兵叫回基地 ✓
    }

    // ---------- 1) 触发（三档）----------
    if (!m_atkOn) {
        // round2#6 【修复·永远不反攻】原来"没见过敌方建筑"就永不启动 ✗
        //   atkPickPoints 本来就有"厂→敌建筑群中心→地图对角"的降级链 ✓
        //   → 给探路一个截止线：22500 帧后不再等情报 ✓
        if (m_siegeSN < 0 && m_enemyBaseX < 0 && f < ATK_MAIN_FRAME) return;
        const bool okEarly = (f >= ATK_EARLY_FRAME && army >= ATK_EARLY_ARMY);
        const bool okMain  = (f >= ATK_MAIN_FRAME && army >= ATK_MAIN_ARMY
                              && (priest == nullptr
                                  || priest->Blood >= priest->MaxBlood * 3 / 5));
        const bool okLast  = (f >= ATK_LAST_FRAME && army >= ATK_LAST_ARMY);
        // 【用户要求·人口满再反攻】主要触发线 = 人口满（50/50 ✓）
        //   人口上限 50 = 农民 20 + 祭司 1 + 军队约 29 → 满了就说明兵养到极限了，该打出去 ✓
        //   原来的 okEarly/okMain（按时间猜）不再作为启动条件 ✓
        //   但保留 okLast（27000 帧 + 10 兵）作为**超时兜底** ✓：人口一直满不了也不会干等判负 ✓
        const bool okFull  = (info.Human_Num >= info.Human_MaxNum);
        // 【策略文档】早开战线：兵力够 + 时间到（第二波过后）→ 立刻集结开打 ✓
        const bool okBows  = (army >= ATK_BOW_ARMY && f >= ATK_BOW_FRAME);
        (void)okEarly;                       // 保留计算，仅不再作为条件（避免"未使用变量"警告 ✓）
        (void)okMain;
        if (!(okBows || okFull || okLast)) return;      // ★早开战线优先（文档 75 行 ✓）
        m_atkOn = 1;
        m_atkPhase = 0;
        m_atkPhaseFrame = f;
        m_assaultSN = -1;
        atkPickPoints(info);
        // 【用户要求·集结点=探路兵首次阵亡点再靠家一点】
        //   阵亡点 = 与敌人的接触线 ✓ 比"基地↔敌营中点"更有战术意义
        //   没死过（探路成功/还没派过）→ 保持 atkPickPoints 算出来的中点 ✓
        if (m_scoutDeathX >= 0) {
            int rHx = m_centerX, rHy = m_centerY;
            if (rHx < 0) rHx = MAP_L / 2;
            if (rHy < 0) rHy = MAP_U / 2;
            const double rvx = (double)(rHx - m_scoutDeathX);
            const double rvy = (double)(rHy - m_scoutDeathY);
            const double rlen = sqrt(rvx * rvx + rvy * rvy);
            if (rlen < 1e-6) {
                m_atkRallyX = m_scoutDeathX; m_atkRallyY = m_scoutDeathY;
            } else if (rlen > ATK_RALLY_HOME_PULL) {
                m_atkRallyX = m_scoutDeathX + (int)(rvx / rlen * ATK_RALLY_HOME_PULL + 0.5);
                m_atkRallyY = m_scoutDeathY + (int)(rvy / rlen * ATK_RALLY_HOME_PULL + 0.5);
            } else {
                m_atkRallyX = rHx; m_atkRallyY = rHy;      // 阵亡点已经离家很近 → 就用家
            }
            if (m_atkRallyX < 2) m_atkRallyX = 2;
            if (m_atkRallyX > MAP_L - 3) m_atkRallyX = MAP_L - 3;
            if (m_atkRallyY < 2) m_atkRallyY = 2;
            if (m_atkRallyY > MAP_U - 3) m_atkRallyY = MAP_U - 3;
        }
        if (m_siegeSN >= 0 && m_siegeX >= 0) {             // 已知厂 → 直接冲着厂去
            m_atkTargetX = m_siegeX;
            m_atkTargetY = m_siegeY;
        }
    }

    // 目标点升级：探路后一旦看到厂，改成冲厂
    if (m_siegeSN >= 0 && m_siegeX >= 0
        && (m_atkTargetX != m_siegeX || m_atkTargetY != m_siegeY)) {
        m_atkTargetX = m_siegeX;
        m_atkTargetY = m_siegeY;
        atkPickPoints(info);
        // 【用户要求·集结点=探路兵首次阵亡点再靠家一点】
        //   阵亡点 = 与敌人的接触线 ✓ 比"基地↔敌营中点"更有战术意义
        //   没死过（探路成功/还没派过）→ 保持 atkPickPoints 算出来的中点 ✓
        if (m_scoutDeathX >= 0) {
            int rHx = m_centerX, rHy = m_centerY;
            if (rHx < 0) rHx = MAP_L / 2;
            if (rHy < 0) rHy = MAP_U / 2;
            const double rvx = (double)(rHx - m_scoutDeathX);
            const double rvy = (double)(rHy - m_scoutDeathY);
            const double rlen = sqrt(rvx * rvx + rvy * rvy);
            if (rlen < 1e-6) {
                m_atkRallyX = m_scoutDeathX; m_atkRallyY = m_scoutDeathY;
            } else if (rlen > ATK_RALLY_HOME_PULL) {
                m_atkRallyX = m_scoutDeathX + (int)(rvx / rlen * ATK_RALLY_HOME_PULL + 0.5);
                m_atkRallyY = m_scoutDeathY + (int)(rvy / rlen * ATK_RALLY_HOME_PULL + 0.5);
            } else {
                m_atkRallyX = rHx; m_atkRallyY = rHy;      // 阵亡点已经离家很近 → 就用家
            }
            if (m_atkRallyX < 2) m_atkRallyX = 2;
            if (m_atkRallyX > MAP_L - 3) m_atkRallyX = MAP_L - 3;
            if (m_atkRallyY < 2) m_atkRallyY = 2;
            if (m_atkRallyY > MAP_U - 3) m_atkRallyY = MAP_U - 3;
        }
        m_atkTargetX = m_siegeX;
        m_atkTargetY = m_siegeY;
    }
    if (m_atkTargetX < 0) return;

    const double tax = atkBD(m_atkTargetX), tay = atkBD(m_atkTargetY);
    const int enemyN = (int)info.enemy_armies.size();

    // ---------- 2) 阶段推进 ----------
    if (m_atkPhase == 0) {
        // 集结：一半以上的兵到了集结点 3 格内 → 进入推进
        int inPlace = 0;
        for (const tagArmy& a : info.armies) {
            if (a.Sort == AT_PRIEST || a.Sort == AT_SCOUT) continue;
            if (atkDist(a.DR, a.UR, atkBD(m_atkRallyX), atkBD(m_atkRallyY))
                <= 3.0 * BLOCKSIDELENGTH) ++inPlace;
        }
        // 【用户要求·等齐了再一起反攻】原来"半数到达"就推进 ✗
        //   → 改成"只剩最后 1 个没到"，或等待超过 ATK_RALLY_WAIT_MAX（防一个兵卡住全队 ✗）
        // 【用户要求·集结一定要等人齐】原来只差最后一个(inPlace >= army-1) ✓ → 改成**全员到齐** ✓
        //   超时放宽到 3000 帧（2 分钟）：防一个兵被地形卡死拖住全队 ✗
        if (army > 0 && (inPlace >= army
                         || f - m_atkPhaseFrame >= ATK_RALLY_WAIT_MAX * 2)) {
            m_atkPhase = 1;
            m_atkPhaseFrame = f;
            atkAdvanceFront(ATK_FRONT_STEP);      // 从集结点先推一格
        }
    }
    if (m_atkPhase == 1 || m_atkPhase == 2) {
        // 【修复·推不动】跳板式前进：部队到齐"当前前压点"3 格内 + 冷却 60 帧
        //   → 前压点再朝目标推 ATK_FRONT_STEP 格（阶段1/2 都生效，能一路推到敌营）
        int atFront = 0;
        for (const tagArmy& a : info.armies) {
            if (a.Sort == AT_PRIEST || a.Sort == AT_SCOUT) continue;
            if (atkDist(a.DR, a.UR, atkBD(m_atkFrontX), atkBD(m_atkFrontY))
                <= 3.0 * BLOCKSIDELENGTH) ++atFront;
        }
        const double frontToTarget = atkDist(atkBD(m_atkFrontX), atkBD(m_atkFrontY), tax, tay);
        // 【用户要求·一层层拉出来打】推进条件两处收紧：
        //   ① 原来是"**半数**到齐"✗ → 一半人先走、另一半追 → 队伍前后两截（"前后冲锋"）✓
        //      改成"**整体到齐**"（只剩最后 1 个没到）✓
        //   ② 前方 ATK_FRONT_CLEAR(12) 格内还有敌人 → **不推进** ✓
        //      → 部队停在前压点，让突击者/远程把这一层逐个拉出来打完，再整体往前走 ✓
        // round2#2 【修复·推不动】原来"前方 12 格必须无敌人"是**无条件**的 ✗
        //   但敌营(31兵5塔)附近前方必然有敌人 → 前压点一步都推不动 ✗
        //   → 只在"离目标还远"时才要求前方清空（一层层拉出来打 ✓ 文档48-50行）
        bool frontClear = true;
        if (frontToTarget > 25.0 * BLOCKSIDELENGTH) {
            for (const tagArmy& fe : info.enemy_armies) {
                if (fe.Blood <= 0) continue;
                if (atkDist(fe.DR, fe.UR, atkBD(m_atkFrontX), atkBD(m_atkFrontY))
                    <= ATK_FRONT_CLEAR * BLOCKSIDELENGTH) { frontClear = false; break; }
            }
        }
        // round2#2b 【修复·无兜底】原来 atFront>=army 恒假就永远不推进（没有任何超时出口）✗
        const bool frontStalled = (f - m_atkPhaseFrame >= ATK_RALLY_WAIT_MAX);   // 60 秒没推进 → 强制前压
        if (army > 0 && (atFront >= army || frontStalled)
            && frontClear
            && f - m_atkPhaseFrame >= ATK_FRONT_GAP
            && frontToTarget > 10.0 * BLOCKSIDELENGTH) {
            atkFrontForward(ATK_FRONT_STEP);
            m_atkPhaseFrame = f;
        }
        if (m_atkPhase == 1) {
            // 敌人靠近目标点 12 格内，或我方已推进到目标 10 格内 → 交战
            bool go = (frontToTarget <= 10.0 * BLOCKSIDELENGTH);
            if (!go) {
                for (const tagArmy& e : info.enemy_armies) {
                    if (e.Blood <= 0) continue;
                    if (atkDist(e.DR, e.UR, tax, tay) <= 12.0 * BLOCKSIDELENGTH) { go = true; break; }
                }
            }
            if (go) { m_atkPhase = 2; m_atkPhaseFrame = f; }
        }
    }
    if (m_atkPhase < 3 && (enemyN <= 4 || f >= ATK_PUSH_FRAME)) {
        m_atkPhase = 3;                      // 大势已去 / 拖太久 → 冲锋
        m_atkPhaseFrame = f;
    }

    // ---------- 3) 给部队下令（只对空闲单位，每帧有限条）----------
    int ordered = 0;
    for (const tagArmy& a : info.armies) {
        if (ordered >= ATK_MAX_ORDER) break;
        if (a.Sort == AT_PRIEST || a.Sort == AT_SCOUT) continue;   // 祭司/侦察兵另行处理
        if (a.SN == m_scoutUnitSN) continue;                        // 探路兵不参与反攻
        if (a.NowState != HUMAN_STATE_IDLE) continue;                // 只下令空闲的，不打断战斗
        if (m_issued.count(a.SN)) continue;

        // ① 附近有敌人 → 直接打最近的（队伍优先打远程兵，保护祭司）
        int target = -1;
        double bestD = 1e18;
        if (m_atkPhase >= 2) {
            int rangedBest = -1;
            double rangedD = 1e18;
            for (const tagArmy& e : info.enemy_armies) {
                if (e.Blood <= 0) continue;
                const double d = atkDist(a.DR, a.UR, e.DR, e.UR);
                if (d > ATK_NEAR_ENEMY * BLOCKSIDELENGTH) continue;
                const bool ranged = (e.Sort == AT_CHARIOT_ARCHER || e.Sort == AT_COMPOSITE_BOWMAN
                                     || e.Sort == AT_BOWMAN || e.Sort == AT_SLINGER);
                if (ranged && d < rangedD) { rangedD = d; rangedBest = e.SN; }
                if (d < bestD) { bestD = d; target = e.SN; }
            }
            if (rangedBest >= 0) target = rangedBest;      // 远程兵优先
        }
        if (target >= 0) {
            self->HumanAction(a.SN, target);
            ++ordered;
            continue;
        }

        // ② 没敌人可打 → 走位：
        //    阶段0 铺在集结区；阶段1/2 铺在前压点；阶段3 直接压向目标点
        // 【修复·格子漂移】格子号改"稳定序号"= 按 SN 比它小的同队单位个数 ✓
        //   原来用顺序计数器 slot ✗ → 帧间遍历顺序一变，同一个兵就换格子 → 来回走 ✗
        int mySlot = 0;
        for (const tagArmy& b : info.armies) {
            if (b.Sort == AT_PRIEST || b.Sort == AT_SCOUT) continue;
            if (b.SN == m_scoutUnitSN) continue;
            if (b.SN < a.SN) ++mySlot;
        }
        int gx, gy;
        // 【修复·被围还冲锋】敌人太多时，"冲锋阶段"按**阶段2**走位 ✓（照常打身边的敌人 ✓）
        // round2#5 【修复·祭司孤军】祭司已在冲锋(阶段3)且有厂目标时，军队必须一起压上替他吸引火力
        //   （文档 65 行："不满血或箭塔密集，要靠军队吸引火力"）
        //   否则整队被降级留守、祭司一个人死在 5 座塔下 = 判负 ✗
        const bool priestCharging = (priest != nullptr && m_atkPhase >= 3 && m_siegeSN >= 0);
        const int effPhase = (m_atkPhase >= 3 && enemyN > ATK_CHARGE_MAXEN && !priestCharging)
                             ? 2 : m_atkPhase;
        if (effPhase == 0) {
            atkSlotPos(m_atkRallyX, m_atkRallyY, ATK_RALLY_R, mySlot, gx, gy);
        } else if (effPhase <= 2) {
            atkSlotPos(m_atkFrontX, m_atkFrontY, ATK_RALLY_R, mySlot, gx, gy);
        } else {
            atkSlotPos(m_atkTargetX, m_atkTargetY, ATK_RALLY_R + 1, mySlot, gx, gy);
        }
        if (atkDist(a.DR, a.UR, atkBD(gx), atkBD(gy)) <= 1.2 * BLOCKSIDELENGTH) continue;  // 到位就别再下令
        self->HumanMove(a.SN, atkBD(gx), atkBD(gy));
        ++ordered;
    }

    // ---------- 4) 突击者拉扯（阶段 1/2）----------
    if (m_atkPhase >= 1 && m_atkPhase <= 2) {
        // 选/换突击者：离敌方目标点最近的兵
        const tagArmy* asUnit = nullptr;
        if (m_assaultSN >= 0) {
            for (const tagArmy& a : info.armies)
                if (a.SN == m_assaultSN) { asUnit = &a; break; }
        }
        if (asUnit == nullptr) {
            m_assaultSN = -1;
            double bd = 1e18;
            for (const tagArmy& a : info.armies) {
                if (a.Sort == AT_PRIEST || a.Sort == AT_SCOUT) continue;
                if (a.SN == m_scoutUnitSN) continue;
                const double d = atkDist(a.DR, a.UR, tax, tay);
                if (d < bd) { bd = d; asUnit = &a; }
            }
            if (asUnit != nullptr) {
                m_assaultSN = asUnit->SN;
                m_assaultPX = asUnit->DR;
                m_assaultPY = asUnit->UR;
            }
        }
        if (asUnit != nullptr && !m_issued.count(asUnit->SN)) {
            // 周围 6 格内有敌人 → 撤回初始位置；否则向敌人方向试探前进
            bool danger = false;
            for (const tagArmy& e : info.enemy_armies) {
                if (e.Blood <= 0) continue;
                if (atkDist(asUnit->DR, asUnit->UR, e.DR, e.UR) <= ATK_ASSAULT_NEAR * BLOCKSIDELENGTH) {
                    danger = true;
                    break;
                }
            }
            // 回到初始位置附近 → 撤掉突击手身份，重新选（文档步骤 3）
            if (atkDist(asUnit->DR, asUnit->UR, m_assaultPX, m_assaultPY) <= 1.5 * BLOCKSIDELENGTH
                && !danger) {
                m_assaultSN = -1;
            } else if (asUnit->NowState == HUMAN_STATE_IDLE) {
                if (danger) self->HumanMove(asUnit->SN, m_assaultPX, m_assaultPY);
                else        self->HumanMove(asUnit->SN, tax, tay);
            }
        }
    }

    // ---------- 5) 祭司 ----------
    if (priest != nullptr && !m_issued.count(priestSN)) {
        const bool lowBlood = (priest->Blood < priest->MaxBlood * 3 / 5);
        // 【修复·编译】canCast 提到块首：后面阶段3 和"交战期/跟随"分支都要用它，
        //   原来它声明在阶段3 的 if 内部 → 别处引用会报 was not declared in this scope ✗
        //   （200 帧保护窗，复用 handlePriest 的 m_convertStartFrame）
        // 【修复·祭司不移动】把"正在施法"与"冷却已好"拆开 ✓
        //   · castWindow：真正的施法保护窗（200 帧）—— 只有它才禁止打扰 ✓
        //   · canCast   ：能不能**发起新转化**（冷却已好 且 不在施法窗）✓
        //   原来两者混在一个 !canCast 里 ✗ → 20 秒冷却（500 帧）期间整个分支空转 → 他一步不动 ✗
        const bool castWindow = (m_convertStartFrame >= 0
                                 && f - m_convertStartFrame < 200);
        const bool canCast = (priest->ConvertCooldown <= 0) && !castWindow;
        if (m_atkPhase >= 3) {
            // 冲锋：贴到厂上转化（唯一的胜利途径）
            if (m_siegeSN >= 0 && m_siegeX >= 0) {
                const double dS = atkDist(priest->DR, priest->UR, atkBD(m_siegeX), atkBD(m_siegeY));
                // 【关键节流】canCast 见块首（200 帧保护窗）：施法期间绝不能再下令，
                //   否则重置转化关系 → 永远转不完。
                if (castWindow) {
                    // 正在施法（200 帧保护窗）→ 本帧完全不碰祭司 ✓
                } else if (dS <= DIS_PRIEST * BLOCKSIDELENGTH) {
                    // ★round2#4 进入转化射程(12格)就交给引擎自己的 Attacking 关系走过去：
                    //   它的 Move 阶段会一直压到"贴邻"(≈1.83格)才施法（引擎核实 ✓）
                    //   这里**绝不能再下 HumanMove** —— 那会 suspendRelation → 2~6 秒施法计时整个作废 ✗
                    if (canCast) {
                        self->HumanAction(priestSN, m_siegeSN);  // ★唯一的取胜调用 ✓
                        m_convertStartFrame = f;
                    }
                } else if (priest->NowState == HUMAN_STATE_IDLE) {
                    self->HumanMove(priestSN, atkBD(m_siegeX), atkBD(m_siegeY));
                }
            } else if (priest->NowState == HUMAN_STATE_IDLE) {
                self->HumanMove(priestSN, tax, tay);             // 还没看到厂 → 跟着推进
            }
        } else if (castWindow) {
            // 【修复·祭司不移动】只有"正在施法"（200 帧保护窗内）才完全不碰他 ✓
            //   冷却期（20 秒）照常走位/跟随 ✓ —— 原来用 !canCast 把冷却期也一起冻住了 ✗
            //   原来 lowBlood 判断排在前面 ✗ → 施法期间血 <60% 就会被下令走位 → 直接取消转化 ✗
        } else if (lowBlood) {
            // 血少 → 退回集结区离敌人最远的那一格
            const int r = ATK_RALLY_R;
            const int sx = m_atkRallyX + ((m_atkRallyX < m_atkTargetX) ? -r : r);
            const int sy = m_atkRallyY + ((m_atkRallyY < m_atkTargetY) ? -r : r);
            if (priest->NowState == HUMAN_STATE_IDLE)
                self->HumanMove(priestSN, atkBD(sx), atkBD(sy));
        } else if (m_atkPhase >= 2) {
            // 交战期：转化射程内血最厚的敌人（最大化削弱敌人、增强自己）
            int best = -1;
            int bestHP = -1;
            for (const tagArmy& e : info.enemy_armies) {
                if (e.Blood <= 0) continue;
                if (e.SN == m_lastConvertedSN) continue;
                const double d = atkDist(priest->DR, priest->UR, e.DR, e.UR);
                if (d > DIS_PRIEST * BLOCKSIDELENGTH) continue;
                if (e.Blood > bestHP) { bestHP = e.Blood; best = e.SN; }
            }
            if (best >= 0 && canCast) {                     // ★冷却没好就不发转化令（否则白发 ✗ 还会重置保护窗 ✗）
                self->HumanAction(priestSN, best);
                m_convertStartFrame = f;                    // 交给 200 帧保护窗
            } else {
                // 【修复·祭司掉队】射程内没敌人可转 → 跟到"前压点后方 6 格"，别冲进最前排
                const int fx = m_atkFrontX + ((m_atkFrontX < m_atkTargetX) ? -6 : 6);
                const int fy = m_atkFrontY + ((m_atkFrontY < m_atkTargetY) ? -6 : 6);
                if (priest->NowState == HUMAN_STATE_IDLE
                    && atkDist(priest->DR, priest->UR, atkBD(fx), atkBD(fy))
                       > 3.0 * BLOCKSIDELENGTH) {
                    self->HumanMove(priestSN, atkBD(fx), atkBD(fy));
                }
            }
        } else if (priest->NowState == HUMAN_STATE_IDLE) {
            // 集结期：留在集结区里离敌人最远的一格
            // 【修复·别和部队挤】部队铺在 ±ATK_RALLY_R 的网格里 ✓ → 祭司再往外 2 格 ✓
            const int r = ATK_RALLY_R + 2;
            const int sx = m_atkRallyX + ((m_atkRallyX < m_atkTargetX) ? -r : r);
            const int sy = m_atkRallyY + ((m_atkRallyY < m_atkTargetY) ? -r : r);
            if (atkDist(priest->DR, priest->UR, atkBD(sx), atkBD(sy)) > 1.5 * BLOCKSIDELENGTH)
                self->HumanMove(priestSN, atkBD(sx), atkBD(sy));
        }
    }

    // ---------- 6) 诊断（每 300 帧一行，stdout）----------
    if (f - m_atkDbgFrame >= 300) {
        m_atkDbgFrame = f;
        std::cout << "[ATK] f=" << f
                  << " on=" << m_atkOn
                  << " phase=" << m_atkPhase
                  << " army=" << army
                  << " enemy=" << enemyN
                  << " siegeSN=" << m_siegeSN
                  << " target=(" << m_atkTargetX << "," << m_atkTargetY << ")"
                  << " rally=(" << m_atkRallyX << "," << m_atkRallyY << ")"
                  << " front=(" << m_atkFrontX << "," << m_atkFrontY << ")"
                  << " assault=" << m_assaultSN
                  << " priestHP=" << (priest ? (int)priest->Blood : -1)
                  << std::endl;
        // 【面板也打一份】同上
        self->DebugText(QString("[ATK]f%1 on%2 ph%3 army%4 enemy%5 siege%6 rally(%7,%8)")
                        .arg(f).arg(m_atkOn).arg(m_atkPhase).arg(army).arg(enemyN)
                        .arg(m_siegeSN).arg(m_atkRallyX).arg(m_atkRallyY));
    }
}


// ===== 【诊断·文件日志】把一行写进 ai_log.txt（相对游戏工作目录 ✓ 追加 ✓ 失败也不影响逻辑 ✓）=====
//   为什么要它：本地跑 GUI 版时 stdout 抓不到（重定向会让引擎崩 ✗），
//   而引擎的 GameLog.log 会把 AI 的消息和 manageOrder 警告粘在一起、吞掉数值 ✗
static void aiLogLine(const QString& s)
{
    std::ofstream ofs("ai_log.txt", std::ios::app);
    if (ofs.is_open()) { ofs << s.toStdString(); ofs << "\n"; }
}
void UsrAI::processData()
{
    tagInfo info = getInfo();       // 每帧获取游戏快照
    m_issued.clear();               // 清空本帧已下令记录
    m_farmTaken.clear();            // 【修复·挤同一块田】本帧农田占用记录也要清
    m_eleSentSN = -1;               // 【大象组队】本帧组队计数清零
    m_eleSentCount = 0;

    // 记录市镇中心坐标（找地/回家参照），首次找到后缓存
    if (m_centerX < 0) {
        for (const tagBuilding& b : info.buildings) {
            if (b.Type == BUILDING_CENTER) { m_centerX = b.BlockDR; m_centerY = b.BlockUR; break; }
        }
    }
    // 维护专职建造者（死亡后重找）
    if (m_builderSN < 0) {
        for (const tagFarmer& f : info.farmers) {
            if (f.FarmerSort == FARMERTYPE_FARMER) { m_builderSN = f.SN; break; }
        }
    } else {
        bool alive = false;
        for (const tagFarmer& f : info.farmers)
            if (f.SN == m_builderSN) { alive = true; break; }
        if (!alive) m_builderSN = -1;
    }

    updateMap(info);                // 建立地图 + 收集已探明空地
    buildBuildings(info);           // 基地建筑：住房→箭塔→兵营→市场→靶场→马厩→学院→农田（专职建造者）
    buildResourceDepots(info);      // 资源点仓库/谷仓：采集者负责（羚羊堆/浆果堆）
    manageCenter(info);             // 市镇中心：升级铜器（优先）→ 生产农民到 20
    manageVillagers(info);          // 农民工作分配（食物优先，动态配额）
    researchTech(info);             // 科技链：谷仓/市场/仓库/兵营/靶场
    trainArmy(info);                // 训练军队（铜器后按 PPT 规划兵种）
    // 第二、三座箭塔：铜器后建，或第二波前 3000 帧就开始建（保证第二波前 3 座塔就位）
    if (info.civilizationStage >= CIVILIZATION_BRONZEAGE || info.GameFrame > FRAME_WAVE2 - 3000) {
        buildArrowTower(info);      // 第二座 +3格 / 第三座 -3格（三座一字排开、间距近）
    }
    scoutPhaseS(this, info);        // 【阶段S·侦察】只动 1 个耗材兵找敌方主营/武器工程厂；排在 defense 前靠 m_issued 让 defense 跳过它
    defense(info);                  // 箭塔"拉仇恨"：优先攻击满血敌人
    handlePriest(info);             // 祭司：贴塔拉怪/转化（优先于探路）
    scoutWithPriest(info);          // 祭司随机探路（若祭司本帧已避险则不执行）
    scoutWithScout(info);           // 侦察骑兵探路（无战事时，持续到第三波前）
    attackPhase(this, info);        // 【反攻】集结→推进拉扯→交战→祭司冲锋转化武器工程厂（排在最后，令生效于本帧末）

    // ===== 【纯日志诊断】每 300 帧(12 秒)一行 + 关键事件（写 ai_log.txt 与 stdout 双通道 ✓）=====
    {
        static int m_stateLogFrame = -9999;
        static int m_lastPriestAlive = -1;
        static int m_lastCenterCnt = -1;
        static bool m_winLogged = false;
        if (info.GameFrame - m_stateLogFrame >= 300) {
            m_stateLogFrame = info.GameFrame;
            int farmerCnt = 0, armyCnt = 0;
            int priestHP = -1, priestCd = -1, priestIdle = -1;
            for (const tagFarmer& f : info.farmers)
                if (f.FarmerSort == FARMERTYPE_FARMER) ++farmerCnt;
            for (const tagArmy& a : info.armies) {
                if (a.Sort == AT_PRIEST) {
                    priestHP = (int)a.Blood; priestCd = (int)a.ConvertCooldown;
                    priestIdle = (a.NowState == HUMAN_STATE_IDLE) ? 1 : 0;
                    continue;
                }
                if (a.Sort == AT_SCOUT) continue;
                ++armyCnt;
            }
            const int centerCnt = countBuilding(info, BUILDING_CENTER);
            const int towerCnt  = countBuilding(info, BUILDING_ARROWTOWER);
            const int rangeCnt  = countBuilding(info, BUILDING_RANGE);
            const int farmCnt   = countBuilding(info, BUILDING_FARM);
            QString line = QString("[STATE] f=%1 | res F%2 W%3 G%4 S%5 | pop %6/%7 | farmers %8 army %9 | age %10")
                           .arg(info.GameFrame).arg(info.Meat).arg(info.Wood).arg(info.Gold)
                           .arg(info.Stone).arg((int)info.Human_Num).arg((int)info.Human_MaxNum)
                           .arg(farmerCnt).arg(armyCnt).arg((int)info.civilizationStage);
            line += QString(" | bld2 Mkt%1 Bks%2 Stb%3 Coll%4 Grn%5")
                    .arg(countBuilding(info, BUILDING_MARKET))
                    .arg(countBuilding(info, BUILDING_ARMYCAMP))
                    .arg(countBuilding(info, BUILDING_STABLE))
                    .arg(countBuilding(info, BUILDING_COLLAGE))
                    .arg(countBuilding(info, BUILDING_GRANARY));
            line += QString(" | bld C%1 T%2 R%3 Fm%4 | scout done%5 unit%6 lost%7 bld%8 expl%9 siegeSN%10")
                    .arg(centerCnt).arg(towerCnt).arg(rangeCnt).arg(farmCnt)   // ★修复：上次替换把这 4 个 .arg 弄丢 → 参数错位 4 位
                    .arg(m_scoutDone).arg(m_scoutUnitSN).arg(m_scoutLost)
                    .arg(m_scoutBldSeen).arg(m_scoutExplPct).arg(m_siegeSN);
            line += QString(" | atk on%1 ph%2 rally %3,%4 tgt %5,%6")
                    .arg(m_atkOn).arg(m_atkPhase).arg(m_atkRallyX).arg(m_atkRallyY)
                    .arg(m_atkTargetX).arg(m_atkTargetY);
            line += QString(" | priest hp%1 cd%2 idle%3")
                    .arg(priestHP).arg(priestCd).arg(priestIdle);
            std::cout << line.toStdString() << std::endl;
            aiLogLine(line);
            if (priestHP < 0 && m_lastPriestAlive != 0) {
                std::cout << "[EVENT] PRIEST_LOST f=" << info.GameFrame << std::endl;
                aiLogLine(QString("[EVENT] PRIEST_LOST f=%1").arg(info.GameFrame));
            }
            m_lastPriestAlive = (priestHP >= 0) ? 1 : 0;
            if (centerCnt == 0 && m_lastCenterCnt != 0) {
                std::cout << "[EVENT] CENTER_LOST f=" << info.GameFrame << std::endl;
                aiLogLine(QString("[EVENT] CENTER_LOST f=%1").arg(info.GameFrame));
            }
            m_lastCenterCnt = centerCnt;
        }
        // 胜利检测：武器工程厂出现在**我方建筑列表**里 = 已被祭司转化 ✓
        //   （tagBuilding 快照没有 isConverted()/isDie() ✗；但 info.buildings 已按归属分好 ✓）
        if (!m_winLogged) {
            for (const tagBuilding& b : info.buildings) {
                if (b.Type == BUILDING_SIEGE) {
                    std::cout << "[EVENT] WIN_FACTORY_CONVERTED f=" << info.GameFrame
                              << " sn=" << b.SN << std::endl;
                    aiLogLine(QString("[EVENT] WIN_FACTORY_CONVERTED f=%1 sn=%2")
                              .arg(info.GameFrame).arg(b.SN));
                    m_winLogged = true;
                    break;
                }
            }
        }
    }

#if USRAI_DEBUG_LINE
    // ===== 【诊断】每 250 帧（10 秒）打一行状态到调试面板 =====
    //   用于定位"第二波没兵"：人口是不是被农民/第一波转化兵占满、食物/黄金够不够
    if (info.GameFrame - m_lastDebugFrame >= 250) {
        m_lastDebugFrame = info.GameFrame;
        int farmerCnt = 0, armyCnt = 0;
        for (const tagFarmer& f : info.farmers)
            if (f.FarmerSort == FARMERTYPE_FARMER) farmerCnt++;
        for (const tagArmy& a : info.armies)
            if (a.Sort != AT_PRIEST && a.Sort != AT_SCOUT) armyCnt++;
        QString why = QStringLiteral("可造兵");
        if (info.civilizationStage < CIVILIZATION_BRONZEAGE)       why = QStringLiteral("未升铜器");
        else if (countBuilding(info, BUILDING_RANGE) == 0)         why = QStringLiteral("靶场未建");
        else if ((int)info.Human_Num >= (int)info.Human_MaxNum)    why = QStringLiteral("人口已满(需补房)");
        else if (info.Meat < BUILDING_RANGE_CREATE_COMPOSITE_BOWMAN_FOOD) why = QStringLiteral("食物不足");
        else if (info.Gold < BUILDING_RANGE_CREATE_COMPOSITE_BOWMAN_GOLD) why = QStringLiteral("黄金不足");
        DebugText(QString(QStringLiteral("AI状态: 人口%1/%2 房%3 农%4 兵%5 食%6 木%7 金%8 农田%9 时代%10 | 造兵:%11"))
                  .arg((int)info.Human_Num).arg((int)info.Human_MaxNum)
                  .arg(countBuilding(info, BUILDING_HOME)).arg(farmerCnt).arg(armyCnt)
                  .arg((int)info.Meat).arg((int)info.Wood).arg((int)info.Gold)
                  .arg(countBuilding(info, BUILDING_FARM)).arg((int)info.civilizationStage)
                  .arg(why));
    }
        DebugText(QString(QStringLiteral("侦察: 兵SN%1 路点%2(%3,%4) 损失%5 敌建筑%6 厂SN%7 探明%8% 完成%9"))
                  .arg(m_scoutUnitSN).arg(m_scoutWPI).arg(m_scoutWPX).arg(m_scoutWPY)
                  .arg(m_scoutLost).arg(m_scoutBldSeen).arg(m_siegeSN)
                  .arg(m_scoutExplPct).arg(m_scoutDone));
#endif   // USRAI_DEBUG_LINE
}
