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
static const int FARM_MAX_COUNT  = 10;   // 约20名村民时，为食物工预留足够的农田
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

// 建造位置检查会在引擎中消耗对象编号，失败不能每帧重试。
struct BuildRequest { int actor,type,x,y,frame; };
static std::map<int,BuildRequest> m_buildRequests;
static std::map<int,int> m_badBuildSite;
static int m_lastBuildAttempt=-9999,m_buildAccepted=0,m_buildRejected=0;
static void maintainBuildRequests(const tagInfo& info)
{
    for(auto it=m_buildRequests.begin();it!=m_buildRequests.end();){
        auto ret=info.ins_ret.find(it->first);
        if(ret!=info.ins_ret.end()){
            if(ret->second!=ACTION_SUCCESS){
                ++m_buildRejected;
                if(ret->second==ACTION_INVALID_HUMANBUILD_OVERLAP
                    ||ret->second==ACTION_INVALID_HUMANBUILD_DIFFERENTHIGH
                    ||ret->second==ACTION_INVALID_HUMANBUILD_UNEXPLORE
                    ||ret->second==ACTION_INVALID_POSITION_NOT_FIT)
                    m_badBuildSite[it->second.x*100+it->second.y]=info.GameFrame;
            }else ++m_buildAccepted;
            it=m_buildRequests.erase(it);
        }else if(info.GameFrame-it->second.frame>500)it=m_buildRequests.erase(it);
        else ++it;
    }
}
static int issueHumanBuild(UsrAI* self,const tagInfo& info,int actor,int type,int x,int y)
{
    if(info.GameFrame-m_lastBuildAttempt<25)return -1;
    auto bad=m_badBuildSite.find(x*100+y);
    if(bad!=m_badBuildSite.end()&&info.GameFrame-bad->second<600)return -1;
    m_lastBuildAttempt=info.GameFrame;
    const int id=self->HumanBuild(actor,type,x,y);
    m_buildRequests[id]={actor,type,x,y,info.GameFrame};
    return id;
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

static int m_priestSurveyX=-1, m_priestSurveyY=-1;
static bool aiTraceEnabled();
// 只从已探明可达地形选经济探索边界；收益是基地周边能新增的视野。
// 不读取隐藏资源坐标，也不依赖随机方向或每帧变化的目标。
static bool economicSurveyPoint(int sx,int sy,Point& target)
{
    if(sx<0||sx>=100||sy<0||sy>=100||m_centerX<0||m_centerY<0)return false;
    int unknown[101][101]={};
    for(int x=0;x<100;++x)for(int y=0;y<100;++y){
        const int gain=m_map[x][y]==-2&&std::hypot(x-m_centerX,y-m_centerY)<=32;
        unknown[x+1][y+1]=gain+unknown[x][y+1]+unknown[x+1][y]-unknown[x][y];
    }
    int distance[10000];std::fill(distance,distance+10000,-1);
    int queue[10000],head=0,tail=0;queue[tail++]=sx*100+sy;distance[sx*100+sy]=0;
    auto open=[](int x,int y){return x>=0&&x<100&&y>=0&&y<100
        &&(m_map[x][y]==0||m_map[x][y]==200);};
    double best=-1e18;bool found=false;
    while(head<tail){
        const int cell=queue[head++],x=cell/100,y=cell%100;
        bool frontier=false;
        for(int dx=-1;dx<=1;++dx)for(int dy=-1;dy<=1;++dy)
            if(x+dx>=0&&x+dx<100&&y+dy>=0&&y+dy<100&&m_map[x+dx][y+dy]==-2)frontier=true;
        const double homeDistance=std::hypot(x-m_centerX,y-m_centerY);
        if(frontier&&m_map[x][y]==0&&distance[cell]>=3&&homeDistance<=26){
            const int x0=std::max(0,x-11),y0=std::max(0,y-11);
            const int x1=std::min(100,x+12),y1=std::min(100,y+12);
            const int gain=unknown[x1][y1]-unknown[x0][y1]-unknown[x1][y0]+unknown[x0][y0];
            const double score=gain-3.0*distance[cell]-homeDistance;
            if(gain>0&&score>best){best=score;target=Point(x,y);found=true;}
        }
        if(distance[cell]>=28)continue;
        for(int dx=-1;dx<=1;++dx)for(int dy=-1;dy<=1;++dy){
            if((!dx&&!dy)||!open(x+dx,y+dy))continue;
            if(dx&&dy&&(!open(x+dx,y)||!open(x,y+dy)))continue;
            const int next=(x+dx)*100+y+dy;
            if(distance[next]>=0)continue;
            distance[next]=distance[cell]+1;queue[tail++]=next;
        }
    }
    return found;
}

// 祭司探路：贴着"探索边界"来回走，让已探明区域一圈圈向外扩散
// 原理：
//   - 只把"旁边还有未探索区域(-2)的空地"当作目标（探索边界）
//   - 角落外围是已探明的海洋(-1)，不是未探索区，所以永远不会选到角落
//   - 每走到一个边界空地，视野(12格)照亮边界外的新区域，边界随之外移
//   - 探路结束（次数用完或到第一波）后，回箭塔/市中心附近待命
void UsrAI::scoutWithPriest(const tagInfo& info)
{
    if (m_atkOn) return; // 反攻期间祭司由护送模块统一控制
    // 1) 找到祭司
    int priestSN = -1;
    const tagArmy* priest = nullptr;
    for (const tagArmy& a : info.armies) {
        if (a.Sort == AT_PRIEST) { priestSN = a.SN; priest = &a; break; }
    }
    if (priest == nullptr) return;                  // 祭司不存在（死亡=游戏失败）
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

    // 已下达的目标持续保留，直到真正到达、目标失效或超时。
    if(m_priestSurveyX>=0){
        const double distance=calDistance(priest->DR,priest->UR,
            (m_priestSurveyX+0.5)*BLOCKSIDELENGTH,(m_priestSurveyY+0.5)*BLOCKSIDELENGTH);
        const bool blocked=m_map[m_priestSurveyX][m_priestSurveyY]!=0
            &&m_map[m_priestSurveyX][m_priestSurveyY]!=200;
        if(distance<=2*BLOCKSIDELENGTH||blocked
            ||(m_scoutStartFrame>=0&&info.GameFrame-m_scoutStartFrame>500)){
            m_priestSurveyX=m_priestSurveyY=-1;m_scoutStartFrame=-1;++m_scoutIdx;
        }
    }
    if(m_priestSurveyX<0){
        Point target;
        if(!economicSurveyPoint(int(priest->DR/BLOCKSIDELENGTH),int(priest->UR/BLOCKSIDELENGTH),target))return;
        m_priestSurveyX=target.x;m_priestSurveyY=target.y;m_scoutStartFrame=info.GameFrame;
        if(aiTraceEnabled())std::cout<<"[ECON_SURVEY] f="<<info.GameFrame<<" target="
            <<target.x<<","<<target.y<<" priest="<<priest->DR/BLOCKSIDELENGTH<<","<<priest->UR/BLOCKSIDELENGTH<<std::endl;
    }
    if(priest->NowState==HUMAN_STATE_IDLE)
        movePriest(priestSN,priest->DR,priest->UR,(m_priestSurveyX+0.5)*BLOCKSIDELENGTH,
            (m_priestSurveyY+0.5)*BLOCKSIDELENGTH,info.GameFrame);
}

// ============================================================
// 侦察骑兵探路：和祭司同样的"探索边界扩散"算法
// 时机：马厩训练出侦察骑兵后自动启用，持续探索到第三波前
// 特点：视野8、速度快、牺牲不致命 → 后期远探（定位敌人位置）
// ============================================================
void UsrAI::scoutWithScout(const tagInfo& info)
{
    if (m_atkOn) return;
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
        // 石、金、鱼占2x2，不能只标左上角，否则建造会一直撞剩下的三格。
        const int size=(r.Type==RESOURCE_STONE||r.Type==RESOURCE_GOLD||r.Type==RESOURCE_FISH)?2:1;
        for(int dx=0;dx<size;++dx)for(int dy=0;dy<size;++dy){
            const int x=r.BlockDR+dx,y=r.BlockUR+dy;
            if(x>=0&&x<100&&y>=0&&y<100)m_map[x][y]=10+r.Type;
        }
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
    auto bad=m_badBuildSite.find(i*100+j);
    if(bad!=m_badBuildSite.end()&&info.GameFrame-bad->second<600)return false;
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
bool UsrAI::findFarmSite(const tagInfo& info, const tagFarmer& farmer, int& x, int& y)
{
    // 农田依靠基地附近的有效粮食交付点；远处狩猎谷仓不能牵走后续农田。
    struct Anchor { int x, y, radius; double cost; };
    std::vector<Anchor> anchors;
    const double fx = farmer.DR / BLOCKSIDELENGTH - 1.5;
    const double fy = farmer.UR / BLOCKSIDELENGTH - 1.5;
    anchors.push_back({m_centerX,m_centerY,12,
        calDistance(fx,fy,m_centerX,m_centerY)});
    for (const auto& b : info.buildings) {
        if (b.Type != BUILDING_GRANARY || b.Percent != 100) continue;
        const double home = calDistance(b.BlockDR,b.BlockUR,m_centerX,m_centerY);
        if (home > 12.0) continue;
        anchors.push_back({b.BlockDR,b.BlockUR,6,
            calDistance(fx,fy,b.BlockDR,b.BlockUR) + home * 0.5});
    }
    std::sort(anchors.begin(),anchors.end(),[](const Anchor& a,const Anchor& b){
        if (a.cost != b.cost) return a.cost < b.cost;
        if (a.x != b.x) return a.x < b.x;
        return a.y < b.y;
    });
    for (const auto& a : anchors) {
        int sx, sy;
        if (findBuildBlockNear(info,sx,sy,3,3,a.x,a.y,a.radius)
            && calDistance(sx,sy,m_centerX,m_centerY) <= 20.0) {
            x = sx; y = sy;
            return true;
        }
    }
    return false;
}

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

// stdout诊断不调用引擎Logger；默认保留低频状态，USRAI_TRACE=0可关闭。
static bool aiTraceEnabled()
{
    static const bool enabled = [](){const char* v=std::getenv("USRAI_TRACE");return !v || v[0]!='0';}();
    return enabled;
}
struct FarmerDamageSnapshot {
    tagFarmer farmer;
    int targetType=-1;
    double targetX=-1,targetY=-1;
};
static std::map<int,FarmerDamageSnapshot> m_farmerDamagePrevious;
static std::vector<tagArmy> m_farmerDamageEnemies,m_farmerDamageOwn;
static std::vector<tagResource> m_farmerDamageBeasts;
static int m_farmerDamageFrame=-1;
static void farmerDamageTrace(const tagInfo& info)
{
    if(!aiTraceEnabled())return;
    if(info.GameFrame<m_farmerDamageFrame){
        m_farmerDamagePrevious.clear();m_farmerDamageEnemies.clear();
        m_farmerDamageOwn.clear();m_farmerDamageBeasts.clear();
    }
    auto threats=[](const FarmerDamageSnapshot& old,const std::vector<tagArmy>& enemies,
                    const std::vector<tagArmy>& own,const std::vector<tagResource>& beasts){
        const auto& f=old.farmer;
        std::cout<<" enemies=";
        for(const auto& e:enemies)if(e.Blood>0&&std::hypot(e.DR-f.DR,e.UR-f.UR)<=12*BLOCKSIDELENGTH)
            std::cout<<e.SN<<":"<<e.Sort<<":"<<e.WorkObjectSN<<":"<<e.DR/BLOCKSIDELENGTH<<","<<e.UR/BLOCKSIDELENGTH<<";";
        std::cout<<" beasts=";
        for(const auto& r:beasts)if(r.Blood>0&&std::hypot(r.DR-f.DR,r.UR-f.UR)<=6*BLOCKSIDELENGTH)
            std::cout<<r.SN<<":"<<r.Type<<":"<<r.Blood<<":"<<r.DR/BLOCKSIDELENGTH<<","<<r.UR/BLOCKSIDELENGTH<<";";
        std::cout<<" ownStone=";
        for(const auto& a:own)if(a.Blood>0&&a.Sort==AT_STONE_THROWER
            &&std::hypot(a.DR-f.DR,a.UR-f.UR)<=12*BLOCKSIDELENGTH)
            std::cout<<a.SN<<":"<<a.WorkObjectSN<<":"<<a.DR/BLOCKSIDELENGTH<<","<<a.UR/BLOCKSIDELENGTH<<";";
    };
    std::map<int,FarmerDamageSnapshot> now;
    for(const auto& f:info.farmers){
        FarmerDamageSnapshot state;state.farmer=f;
        auto previous=m_farmerDamagePrevious.find(f.SN);
        if(previous!=m_farmerDamagePrevious.end()&&previous->second.farmer.WorkObjectSN==f.WorkObjectSN){
            state.targetType=previous->second.targetType;
            state.targetX=previous->second.targetX;state.targetY=previous->second.targetY;
        }
        for(const auto& r:info.resources)if(r.SN==f.WorkObjectSN){
            state.targetType=r.Type;state.targetX=r.DR/BLOCKSIDELENGTH;state.targetY=r.UR/BLOCKSIDELENGTH;break;
        }
        for(const auto& b:info.buildings)if(b.SN==f.WorkObjectSN){
            state.targetType=1000+b.Type;state.targetX=b.BlockDR;state.targetY=b.BlockUR;break;
        }
        if(previous!=m_farmerDamagePrevious.end()&&f.Blood<previous->second.farmer.Blood){
            const auto& old=previous->second;
            std::cout<<"[FARMER_HIT] f="<<info.GameFrame<<" sn="<<f.SN<<" hp="<<old.farmer.Blood<<"->"<<f.Blood
                <<" pos="<<f.DR/BLOCKSIDELENGTH<<","<<f.UR/BLOCKSIDELENGTH
                <<" state="<<f.NowState<<" role="<<m_role[f.SN]<<" work="<<f.WorkObjectSN
                <<" target="<<state.targetType<<":"<<state.targetX<<","<<state.targetY<<" carry="<<f.Resource;
            threats(state,info.enemy_armies,info.armies,m_farmerDamageBeasts);std::cout<<std::endl;
        }
        now[f.SN]=state;
    }
    for(const auto& entry:m_farmerDamagePrevious)if(!now.count(entry.first)){
        const auto& old=entry.second;const auto& f=old.farmer;
        std::cout<<"[FARMER_LOSS] f="<<info.GameFrame<<" previous="<<m_farmerDamageFrame
            <<" sn="<<f.SN<<" hp="<<f.Blood<<" pos="<<f.DR/BLOCKSIDELENGTH<<","<<f.UR/BLOCKSIDELENGTH
            <<" state="<<f.NowState<<" role="<<m_role[f.SN]<<" work="<<f.WorkObjectSN
            <<" target="<<old.targetType<<":"<<old.targetX<<","<<old.targetY<<" carry="<<f.Resource;
        threats(old,m_farmerDamageEnemies,m_farmerDamageOwn,m_farmerDamageBeasts);std::cout<<std::endl;
    }
    m_farmerDamagePrevious.swap(now);m_farmerDamageFrame=info.GameFrame;
    m_farmerDamageEnemies=info.enemy_armies;m_farmerDamageOwn=info.armies;
    m_farmerDamageBeasts.clear();
    for(const auto& r:info.resources)if(r.Blood>0&&(r.Type==RESOURCE_ELEPHANT||r.Type==RESOURCE_LION))m_farmerDamageBeasts.push_back(r);
}
// 村民自行建田的指令可能先行走再出现地基，必须跨帧保留建造任务。
struct FarmBuildOrder { int x,y,frame,id,progressFrame,percent; double x0,y0; };
static std::map<int,FarmBuildOrder> m_farmBuildOrders;
static int farmSites(const tagInfo& info)
{
    int n=0;
    for(const auto& b:info.buildings)if(b.Type==BUILDING_FARM)++n;
    for(const auto& o:m_farmBuildOrders){
        bool present=false;
        for(const auto& b:info.buildings)if(b.Type==BUILDING_FARM
            &&b.BlockDR==o.second.x&&b.BlockUR==o.second.y)present=true;
        if(!present)++n;
    }
    return n;
}
static int farmBuild(UsrAI* self,const tagInfo& info,const tagFarmer& f,int x,int y)
{
    const int id=issueHumanBuild(self,info,f.SN,BUILDING_FARM,x,y);
    if(id<0)return id;
    m_farmBuildOrders[f.SN]={x,y,info.GameFrame,id,info.GameFrame,0,f.DR,f.UR};
    m_role[f.SN]=5;m_foodGatherers.insert(f.SN);
    return id;
}
static void maintainFarmBuilds(const tagInfo& info)
{
    for(auto it=m_farmBuildOrders.begin();it!=m_farmBuildOrders.end();){
        const tagFarmer* f=nullptr;
        for(const auto& a:info.farmers)if(a.SN==it->first){f=&a;break;}
        auto ret=info.ins_ret.find(it->second.id);
        if(!f || (ret!=info.ins_ret.end() && ret->second!=ACTION_SUCCESS)){
            it=m_farmBuildOrders.erase(it);continue;
        }
        int percent=0;bool done=false;
        for(const auto& b:info.buildings)if(b.Type==BUILDING_FARM
            &&b.BlockDR==it->second.x && b.BlockUR==it->second.y){percent=b.Percent;done=b.Percent>=100;break;}
        if(done){it=m_farmBuildOrders.erase(it);continue;}
        if(percent>it->second.percent || std::hypot(f->DR-it->second.x0,f->UR-it->second.y0)>BLOCKSIDELENGTH){
            it->second.progressFrame=info.GameFrame;it->second.percent=percent;
            it->second.x0=f->DR;it->second.y0=f->UR;
        }
        if(info.GameFrame-it->second.progressFrame>1500){it=m_farmBuildOrders.erase(it);continue;}
        ++it;
    }
}

struct EconOrder { int farmer, target, frame; };
static std::map<int,EconOrder> m_econOrders;
static std::map<int,int> m_econReturns;
static int m_econSent=0,m_econUnknown=0,m_econReport=-9999,m_econErrorReport=-9999;
static int econAction(UsrAI* self,const tagInfo& info,int farmer,int target)
{
    const int id=self->HumanAction(farmer,target);
    {
        while(m_econOrders.size()>=128){m_econOrders.erase(m_econOrders.begin());++m_econUnknown;}
        m_econOrders[id]={farmer,target,info.GameFrame};++m_econSent;
    }
    return id;
}
static void econTrace(const tagInfo& info)
{
    for(auto it=m_econOrders.begin();it!=m_econOrders.end();) {
        auto ret=info.ins_ret.find(it->first);
        if(ret!=info.ins_ret.end()) {
            ++m_econReturns[ret->second];
            if(ret->second==ACTION_INVALID_OBSN || ret->second==ACTION_INVALID_NULLGOALOBJECT)
                m_badTarget[it->second.target]=info.GameFrame;
            if(aiTraceEnabled() && ret->second!=ACTION_SUCCESS && info.GameFrame-m_econErrorReport>=100) {
                m_econErrorReport=info.GameFrame;
                std::cout<<"[ECON_RET] f="<<info.GameFrame<<" id="<<it->first
                    <<" farmer="<<it->second.farmer<<" target="<<it->second.target<<" ret="<<ret->second<<std::endl;
            }
            it=m_econOrders.erase(it);
        } else if(info.GameFrame-it->second.frame>500){++m_econUnknown;it=m_econOrders.erase(it);}
        else ++it;
    }
    if(!aiTraceEnabled()) { m_econReturns.clear();m_econSent=0;m_econUnknown=0;return; }
    if(info.GameFrame-m_econReport<750)return;
    if(m_econReport<0)std::cout<<"[ECON_ENV] version="<<GAME_VERSION<<" block="<<double(BLOCKSIDELENGTH)
        <<" treeType="<<RESOURCE_TREE<<" bushType="<<RESOURCE_BUSH<<" goldType="<<RESOURCE_GOLD<<std::endl;
    m_econReport=info.GameFrame;
    int idle=0,walking=0,working=0,loaded=0;
    for(const auto& f:info.farmers){
        idle+=f.NowState==HUMAN_STATE_IDLE;walking+=f.NowState==HUMAN_STATE_WALKING;
        working+=f.NowState==HUMAN_STATE_WORKING;loaded+=f.Resource>0;
    }
    std::cout<<"[ECON] f="<<info.GameFrame<<" age="<<info.civilizationStage
        <<" stock="<<info.Meat<<","<<info.Wood<<","<<info.Gold
        <<" states="<<idle<<","<<walking<<","<<working<<" loaded="<<loaded
        <<" farmSites="<<farmSites(info)<<" farmBuilders="<<m_farmBuildOrders.size()
        <<" buildOK="<<m_buildAccepted<<" buildFail="<<m_buildRejected
        <<" sent="<<m_econSent<<" unknown="<<m_econUnknown<<" pending="<<m_econOrders.size()<<" returns=";
    for(const auto& r:m_econReturns)std::cout<<r.first<<":"<<r.second<<",";
    std::cout<<std::endl;
    std::map<int,int> counts,positive;std::set<int> samples;
    for(const auto& r:info.resources){++counts[r.Type];positive[r.Type]+=r.Cnt>0;}
    std::cout<<"[ECON_RES] types=";
    for(const auto& r:counts)std::cout<<r.first<<":"<<r.second<<"/"<<positive[r.first]<<",";
    std::cout<<std::endl;
    for(const auto& r:info.resources)if(samples.insert(r.Type).second)
        std::cout<<"[ECON_SAMPLE] type="<<r.Type<<" sn="<<r.SN<<" cnt="<<r.Cnt
            <<" blood="<<r.Blood<<" block="<<r.BlockDR<<","<<r.BlockUR
            <<" pos="<<r.DR/BLOCKSIDELENGTH<<","<<r.UR/BLOCKSIDELENGTH<<std::endl;
    for(const auto& b:info.buildings)if(b.Type==BUILDING_FARM){
        int users=0;for(const auto& f:info.farmers)if(f.WorkObjectSN==b.SN&&f.NowState!=HUMAN_STATE_IDLE)++users;
        std::cout<<"[ECON_FARM] sn="<<b.SN<<" percent="<<b.Percent<<" cnt="<<b.Cnt
            <<" users="<<users<<" pos="<<b.BlockDR<<","<<b.BlockUR<<std::endl;
    }
    for(const auto& b:info.buildings)if(b.Type!=BUILDING_FARM)
        std::cout<<"[ECON_BUILD] sn="<<b.SN<<" type="<<b.Type<<" percent="<<b.Percent
            <<" pos="<<b.BlockDR<<","<<b.BlockUR<<std::endl;
    for(const auto& f:info.farmers)
        std::cout<<"[ECON_WORKER] sn="<<f.SN<<" state="<<f.NowState<<" work="<<f.WorkObjectSN
            <<" carry="<<f.Resource<<" kind="<<f.ResourceSort
            <<" pos="<<f.DR/BLOCKSIDELENGTH<<","<<f.UR/BLOCKSIDELENGTH<<std::endl;
    int n=0;
    for(const auto& f:info.farmers)if(f.NowState==HUMAN_STATE_IDLE && n++<4){
        std::cout<<"[ECON_IDLE] sn="<<f.SN<<" role="<<m_role[f.SN]<<" work="<<f.WorkObjectSN
            <<" carry="<<f.Resource<<" kind="<<f.ResourceSort<<" pos="<<f.DR/BLOCKSIDELENGTH<<","<<f.UR/BLOCKSIDELENGTH;
        const auto target=m_orderTarget.find(f.SN),order=m_orderFrame.find(f.SN);
        if(target!=m_orderTarget.end())std::cout<<" orderTarget="<<target->second;
        if(order!=m_orderFrame.end())std::cout<<" orderAge="<<info.GameFrame-order->second;
        std::cout<<std::endl;
    }
    for(const auto& f:info.farmers)if(m_role[f.SN]==3){
        const tagResource* mine=nullptr;
        for(const auto& r:info.resources)if(r.SN==f.WorkObjectSN){mine=&r;break;}
        std::cout<<"[ECON_GOLD] sn="<<f.SN<<" state="<<f.NowState<<" work="<<f.WorkObjectSN
            <<" carry="<<f.Resource<<" kind="<<f.ResourceSort
            <<" pos="<<f.DR/BLOCKSIDELENGTH<<","<<f.UR/BLOCKSIDELENGTH;
        if(mine)std::cout<<" target="<<mine->DR/BLOCKSIDELENGTH<<","<<mine->UR/BLOCKSIDELENGTH
            <<" cnt="<<mine->Cnt<<" type="<<mine->Type;
        std::cout<<std::endl;
    }
    for(const auto& b:info.buildings)if(b.Type==BUILDING_STOCK)
        std::cout<<"[ECON_STOCK] sn="<<b.SN<<" percent="<<b.Percent
            <<" pos="<<b.BlockDR<<","<<b.BlockUR<<std::endl;
    m_econSent=m_econUnknown=0;m_econReturns.clear();
}


// 铜器经济进入持续生产阶段后，让实际工种跟上资源预算。
// 一次只调一人；携带资源返仓、建造及本帧已有任务的农民不被打断。
static bool unfinishedFarmReachable(const tagFarmer& farmer,const tagBuilding& farm)
{
    const int sx=int(farmer.DR/BLOCKSIDELENGTH),sy=int(farmer.UR/BLOCKSIDELENGTH);
    if(sx<1||sy<1||sx>=99||sy>=99)return false;
    bool seen[10000]={false};int queue[10000],head=0,tail=0;
    queue[tail++]=sx*100+sy;seen[sx*100+sy]=true;
    auto open=[](int x,int y){return x>=1&&y>=1&&x<99&&y<99
        &&(m_map[x][y]==0||m_map[x][y]==200);};
    while(head<tail){
        const int at=queue[head++],x=at/100,y=at%100;
        if(((x==farm.BlockDR-1||x==farm.BlockDR+3)&&y>=farm.BlockUR&&y<farm.BlockUR+3)
            ||((y==farm.BlockUR-1||y==farm.BlockUR+3)&&x>=farm.BlockDR&&x<farm.BlockDR+3))return true;
        for(int dx=-1;dx<=1;++dx)for(int dy=-1;dy<=1;++dy){
            const int nx=x+dx,ny=y+dy;
            if((!dx&&!dy)||!open(nx,ny)||std::hypot(nx-sx,ny-sy)>28)continue;
            if(dx&&dy&&(!open(x+dx,y)||!open(x,y+dy)))continue;
            const int index=nx*100+ny;
            if(!seen[index]){seen[index]=true;queue[tail++]=index;}
        }
    }
    return false;
}
static int m_resumeFarmFrame=-9999;
bool UsrAI::resumeFarmConstruction(const tagInfo& info)
{
    if(info.civilizationStage<CIVILIZATION_BRONZEAGE || info.Meat>=250
        || info.GameFrame-m_resumeFarmFrame<25)return false;
    m_resumeFarmFrame=info.GameFrame;
    const tagFarmer* worker=nullptr;const tagBuilding* farm=nullptr;double best=1e18;
    for(const auto& b:info.buildings){
        if(b.Type!=BUILDING_FARM || b.Percent>=100 || isBadTarget(b.SN,info.GameFrame))continue;
        bool assigned=false;
        for(const auto& f:info.farmers)if(f.NowState!=HUMAN_STATE_IDLE && f.WorkObjectSN==b.SN)assigned=true;
        for(const auto& o:m_farmBuildOrders)if(o.second.x==b.BlockDR&&o.second.y==b.BlockUR)assigned=true;
        if(assigned)continue;
        for(const auto& f:info.farmers){
            if(f.FarmerSort!=FARMERTYPE_FARMER || f.Blood<=0 || f.Resource>0
                || f.NowState!=HUMAN_STATE_IDLE || f.SN==m_builderSN || f.SN==m_depotBuilderSN
                || m_issued.count(f.SN) || m_farmBuildOrders.count(f.SN))continue;
            const double score=calDistance(f.DR,f.UR,(b.BlockDR+1.5)*BLOCKSIDELENGTH,
                (b.BlockUR+1.5)*BLOCKSIDELENGTH)/BLOCKSIDELENGTH+(100-b.Percent)*0.08;
            if(score<best && unfinishedFarmReachable(f,b)){best=score;worker=&f;farm=&b;}
        }
    }
    if(!worker)return false;
    const int id=econAction(this,info,worker->SN,farm->SN);
    if(id<0)return false;
    m_farmBuildOrders[worker->SN]={farm->BlockDR,farm->BlockUR,info.GameFrame,id,
        info.GameFrame,farm->Percent,worker->DR,worker->UR};
    m_role[worker->SN]=5;m_foodGatherers.insert(worker->SN);m_issued.insert(worker->SN);
    if(aiTraceEnabled())std::cout<<"[FARM_RESUME] f="<<info.GameFrame<<" farmer="<<worker->SN
        <<" farm="<<farm->SN<<" percent="<<farm->Percent<<std::endl;
    return true;
}

// 有效采集与正常返仓期间，其他模块不打断携带资源的农民。
// 已空闲或任务已失效者继续走正常分配，不能因强制交货而永久冻结。
static void preserveFarmerCargo(UsrAI*,const tagInfo& info)
{
    for(const auto& f:info.farmers){
        if(f.FarmerSort!=FARMERTYPE_FARMER || f.Blood<=0 || f.Resource<=0
            || f.NowState==HUMAN_STATE_IDLE)continue;
        bool valid=false;
        for(const auto& r:info.resources)if(r.SN==f.WorkObjectSN && r.Cnt>0)valid=true;
        for(const auto& b:info.buildings)if(b.SN==f.WorkObjectSN
            && b.Type==BUILDING_FARM && b.Percent==100 && b.Cnt>0)valid=true;
        if(valid)m_issued.insert(f.SN);
    }
}

// 空闲载货者先交仓，不能直接改采集来源清掉尚未交付的资源。
struct CargoDelivery { int target, frame, progress; double x,y,amount; };
static std::map<int,CargoDelivery> m_cargoDelivery;
static std::map<int,std::map<int,int>> m_cargoDepotBad;
static int m_cargoFrame=-1;
static bool cargoDepotMatches(int type,int kind)
{
    return type==BUILDING_CENTER
        || (type==BUILDING_GRANARY && kind==HUMAN_GRANARYFOOD)
        || (type==BUILDING_STOCK && (kind==HUMAN_WOOD || kind==HUMAN_GOLD
            || kind==HUMAN_STONE || kind==HUMAN_STOCKFOOD));
}
static void recoverIdleCargo(UsrAI* self,const tagInfo& info)
{
    if(info.GameFrame<m_cargoFrame){m_cargoDelivery.clear();m_cargoDepotBad.clear();}
    m_cargoFrame=info.GameFrame;
    for(const auto& f:info.farmers){
        if(f.FarmerSort!=FARMERTYPE_FARMER || f.Blood<=0)continue;
        auto it=m_cargoDelivery.find(f.SN);
        if(f.Resource<=0){m_cargoDelivery.erase(f.SN);m_cargoDepotBad.erase(f.SN);continue;}
        if(it!=m_cargoDelivery.end()){
            auto& o=it->second;
            if(std::hypot(f.DR-o.x,f.UR-o.y)>0.5*BLOCKSIDELENGTH || f.Resource!=o.amount){
                o.x=f.DR;o.y=f.UR;o.amount=f.Resource;o.progress=info.GameFrame;
            }
            if(info.GameFrame-o.progress<150){m_issued.insert(f.SN);continue;}
            m_cargoDepotBad[f.SN][o.target]=info.GameFrame;
            m_cargoDelivery.erase(it);
        }else if(f.NowState!=HUMAN_STATE_IDLE)continue;
        // 只保护短期交货命令。失败后换匹配仓库，不永久保护一个卡住任务。
        const tagBuilding* depot=nullptr;double best=1e18;
        for(const auto& b:info.buildings){
            if(b.Percent!=100 || b.Blood<=0 || !cargoDepotMatches(b.Type,f.ResourceSort))continue;
            auto bad=m_cargoDepotBad[f.SN].find(b.SN);
            if(bad!=m_cargoDepotBad[f.SN].end() && info.GameFrame-bad->second<600)continue;
            const double d=std::hypot(f.DR-(b.BlockDR+1.5)*BLOCKSIDELENGTH,
                f.UR-(b.BlockUR+1.5)*BLOCKSIDELENGTH);
            if(d<best){best=d;depot=&b;}
        }
        if(!depot)continue;
        const int id=econAction(self,info,f.SN,depot->SN);
        if(id<0)continue;
        m_cargoDelivery[f.SN]={depot->SN,info.GameFrame,info.GameFrame,f.DR,f.UR,double(f.Resource)};
        m_issued.insert(f.SN);
        if(aiTraceEnabled())std::cout<<"[CARGO_DELIVER] f="<<info.GameFrame<<" sn="<<f.SN
            <<" carry="<<f.Resource<<" kind="<<f.ResourceSort<<" depot="<<depot->SN<<std::endl;
    }
}

static int m_foodRebalanceFrame = -9999;
bool UsrAI::rebalanceFood(const tagInfo& info, int& foodCnt, int& woodCnt, int& goldCnt,
                         int targetFood, int targetWood, int targetGold)
{
    if (info.civilizationStage < CIVILIZATION_BRONZEAGE || info.Meat >= 180
        || foodCnt >= targetFood || info.GameFrame - m_foodRebalanceFrame < 50) return false;
    // 严重缺粮时不必等库存堆到200木/180金才调回超额工人。
    const int spareWoodGate=info.Meat<60?150:200;
    const int spareGoldGate=info.Meat<60?20:180;
    const tagFarmer* selected = nullptr;
    int selectedTarget = -1, selectedRole = 0, oldRole = 0;
    double best = 1e18;
    std::map<int,int> users;
    for (const auto& f : info.farmers)
        if (f.NowState == HUMAN_STATE_WORKING || f.NowState == HUMAN_STATE_WALKING)
            ++users[f.WorkObjectSN];
    for (const auto& f : info.farmers) {
        if (f.FarmerSort != FARMERTYPE_FARMER || f.Blood <= 0 || f.Resource > 0
            || f.SN == m_builderSN || f.SN == m_depotBuilderSN
            || m_farmBuildOrders.count(f.SN) || m_issued.count(f.SN)) continue;
        if (f.NowState != HUMAN_STATE_WORKING && f.NowState != HUMAN_STATE_WALKING) continue;
        int role = 0;
        for (const auto& r : info.resources) if (r.SN == f.WorkObjectSN && r.Cnt > 0) {
            if (r.Type == RESOURCE_GOLD && goldCnt > targetGold && info.Gold >= spareGoldGate) role = 3;
            if (r.Type == RESOURCE_TREE && woodCnt > targetWood && info.Wood >= spareWoodGate) role = 2;
            break;
        }
        if (!role) continue;
        const int farm = findNearestFarm(info, f.SN);
        for (const auto& b : info.buildings) if (b.SN == farm) {
            const double score = calDistance(f.DR, f.UR,
                (b.BlockDR + 1.5) * BLOCKSIDELENGTH, (b.BlockUR + 1.5) * BLOCKSIDELENGTH);
            if (score < best) { best = score; selected = &f; selectedTarget = farm; selectedRole = 5; oldRole = role; }
        }
        // 已杀死的猎物可直接供粮；不为调工再开一场组队狩猎。
        for (const auto& r : info.resources) {
            const bool berry = r.Type == RESOURCE_BUSH;
            const bool meat = (r.Type == RESOURCE_GAZELLE || r.Type == RESOURCE_ELEPHANT
                              || r.Type == RESOURCE_LION) && r.Blood <= 0;
            if ((!berry && !meat) || r.Cnt <= 0 || isBadTarget(r.SN,info.GameFrame)
                || !resSafe(info,r)) continue;
            const int cap = r.Type == RESOURCE_ELEPHANT ? 3 : (r.Type == RESOURCE_LION ? 1 : 2);
            if (users[r.SN] >= cap) continue;
            double delivery = 1e18;
            for (const auto& b : info.buildings) if (b.Percent == 100
                && (b.Type == BUILDING_CENTER || b.Type == (berry ? BUILDING_GRANARY : BUILDING_STOCK)))
                delivery = std::min(delivery, calDistance(r.DR,r.UR,
                    (b.BlockDR + 1.5) * BLOCKSIDELENGTH, (b.BlockUR + 1.5) * BLOCKSIDELENGTH));
            const double score = calDistance(f.DR,f.UR,r.DR,r.UR) + 2 * delivery;
            if (score < best) { best = score; selected = &f; selectedTarget = r.SN; selectedRole = berry ? 1 : 4; oldRole = role; }
        }
    }
    bool built = false;
    // 现有粮源满员时，富余工人直接建立自己的田，形成持续供粮能力。
    const int desiredFarms = std::min(FARM_MAX_COUNT, std::max(FARM_MIN_COUNT,
        info.GameFrame > FRAME_WAVE2 ? int(info.farmers.size()) / 2 : 6));
    const int woodGate = std::max(info.Meat < 150 ? 80 : FARM_WOOD_GATE,
        m_researchCount[BUILDING_RANGE_UPGRADE_COMPOSITE_BOW] == 0
            ? BUILDING_RANGE_UPGRADE_COMPOSITE_BOW_WOOD + 75 : 0);
    if (!selected && countBuilding(info,BUILDING_MARKET) > 0
        && farmSites(info) < desiredFarms && info.Wood >= woodGate) {
        double nearest = 1e18;
        for (const auto& f : info.farmers) {
            if (f.FarmerSort != FARMERTYPE_FARMER || f.Blood <= 0 || f.Resource > 0
                || f.SN == m_builderSN || f.SN == m_depotBuilderSN
                || m_farmBuildOrders.count(f.SN) || m_issued.count(f.SN)) continue;
            if (f.NowState != HUMAN_STATE_WORKING && f.NowState != HUMAN_STATE_WALKING) continue;
            int role = 0;
            for (const auto& r : info.resources) if (r.SN == f.WorkObjectSN && r.Cnt > 0) {
                if (r.Type == RESOURCE_GOLD && goldCnt > targetGold && info.Gold >= spareGoldGate) role = 3;
                if (r.Type == RESOURCE_TREE && woodCnt > targetWood && info.Wood >= spareWoodGate) role = 2;
                break;
            }
            const double distance = calDistance(f.DR,f.UR,
                (m_centerX + 1.5) * BLOCKSIDELENGTH, (m_centerY + 1.5) * BLOCKSIDELENGTH);
            if (role && distance < nearest) { nearest = distance; selected = &f; oldRole = role; }
        }
        if (selected) {
            int x, y;
            if (!findFarmSite(info,*selected,x,y)
                || farmBuild(this,info,*selected,x,y) < 0) return false;
            selectedRole = 5; built = true;
        }
    }
    if (!selected) return false;
    if (!built) {
        if (econAction(this,info,selected->SN,selectedTarget) < 0) return false;
        if (selectedRole == 5) m_farmTaken.insert(selectedTarget);
        m_orderTarget[selected->SN] = selectedTarget;
        m_orderFrame[selected->SN] = info.GameFrame;
        m_orderX[selected->SN] = selected->DR; m_orderY[selected->SN] = selected->UR;
    }
    m_issued.insert(selected->SN); m_foodGatherers.insert(selected->SN);
    m_role[selected->SN] = selectedRole;
    if (oldRole == 2) --woodCnt;
    if (oldRole == 3) --goldCnt;
    ++foodCnt; m_foodRebalanceFrame = info.GameFrame;
    if (aiTraceEnabled()) std::cout << "[ECON_SHIFT] f=" << info.GameFrame << " farmer=" << selected->SN
        << " from=" << oldRole << " to=" << selectedRole << " target=" << selectedTarget << " build=" << built << std::endl;
    return built;
}

// 靶场建成后的持续供粮预算；建造工另留名额，三个配额不超过实际劳动力。
static void bronzeWorkerTargets(int total,int reserved,int food,int wood,int gold,
                                int& targetFood,int& targetWood,int& targetGold)
{
    const int available=std::max(0,total-reserved);
    targetFood=std::min(available,std::max(6,(available+1)/2));
    // 粮食库存足够而黄金紧缺时，暂借两名食物工；仍保留至少6名供粮工和2名伐木工。
    if(food>=250 && gold<100)targetFood=std::min(available,std::max(6,targetFood-2));
    const int desiredGold=gold>=300?2:(gold<100?(food>=250?7:(food>=80?5:4)):3);
    targetGold=std::min(desiredGold,std::max(0,available-targetFood-2));
    const int desiredWood=wood<100?6:(wood<200?5:3);
    targetWood=std::min(desiredWood,available-targetFood-targetGold);
    targetFood=available-targetWood-targetGold;
}

static int m_goldRebalanceFrame=-9999;
void UsrAI::manageVillagers(const tagInfo& info)
{
    maintainFarmBuilds(info);
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
    if (info.Wood < 150) targetWood = 4;        // 木头不足：加 1 人
    if (info.Wood < 60) targetWood = 5;         // 严重不足：再加 1 人
    // 【发育策略】不派挖石工：初始 150 石正好建 1 座塔（塔上限 1 座），人力全给食物/木头/黄金
    int targetStone = 0;
    // 【3.0.7g 调整】金矿 200→400（翻倍）→ 黄金更充裕，挖金保持 3 人
    // 【发育策略】3 人采金：采金不占食物预算，且铜器后造兵急用黄金；多余农民优先采金而非伐木
    // 【用户要求·3.0.7g】升级铜器前**不采黄金**：原来 3 个采金的人先去采食物/木材；
    //   一旦铜器升级已经开始（市中心正在升级，TIME_BUILDING_CENTER_UPGRADE=60 秒）
    //   → 按升级进度"陆续"派人去采金：0 人 → 1 → 2 → 3 人；升完铜器后固定 3 人。
    int targetGold = 3;
    if (info.civilizationStage < CIVILIZATION_BRONZEAGE) {
        if (m_bronzeUpgradeFrame < 0) {
            targetGold = 0;                        // 还没开始升级 → 一个都不去采金
        } else {
            int elapsed = info.GameFrame - m_bronzeUpgradeFrame;
            targetGold = 1 + elapsed / 500;        // 每 20 秒加 1 人（升级总时长 60 秒）
            if (targetGold > 3) targetGold = 3;
        }
    }

    if (bronze) {
        if (countBuilding(info, BUILDING_RANGE) == 0) {
            // 首个靶场的建筑链仍优先攒木，避免食物预算拖慢科技解锁。
            targetWood = total >= 14 ? 10 : 6;
            targetGold = 0;
            targetFood = std::max(4,total-targetWood);
        } else {
            int reserved=0;
            for(const auto& f:info.farmers)
                if(f.FarmerSort==FARMERTYPE_FARMER
                    && (f.SN==m_builderSN || f.SN==m_depotBuilderSN))++reserved;
            bronzeWorkerTargets(total,reserved,info.Meat,info.Wood,info.Gold,
                                targetFood,targetWood,targetGold);
        }
    }

    if (bronze && goldCnt < targetGold && info.GameFrame-m_goldRebalanceFrame>=50) {
        for (const auto& f : info.farmers) {
            if (f.FarmerSort != FARMERTYPE_FARMER || f.SN == m_builderSN
                || f.SN == m_depotBuilderSN || f.Resource > 0 || f.Blood <= 0
                || m_farmBuildOrders.count(f.SN) || m_issued.count(f.SN) || m_role[f.SN] == 3) continue;
            const int role = m_role[f.SN];
            const bool foodRole = role == 1 || role == 4 || role == 5;
            if (f.NowState != HUMAN_STATE_IDLE && !(role == 2 && woodCnt > targetWood)
                && !(foodRole && foodCnt > targetFood)) continue;
            bool constructing = false;
            for (const auto& b : info.buildings)
                if (b.SN == f.WorkObjectSN && b.Percent < 100) constructing = true;
            if (constructing) continue;
            const int gold = findNearestResource(info, RESOURCE_GOLD, f.SN);
            if (gold < 0) continue;
            if(econAction(this,info,f.SN,gold)<0)continue;
            m_issued.insert(f.SN);
            m_foodGatherers.erase(f.SN); m_role[f.SN] = 3;
            m_orderTarget[f.SN] = gold; m_orderFrame[f.SN] = info.GameFrame;
            m_orderX[f.SN] = f.DR; m_orderY[f.SN] = f.UR;
            if (role == 2) --woodCnt;
            if (foodRole) --foodCnt;
            ++goldCnt;m_goldRebalanceFrame=info.GameFrame;
            if(aiTraceEnabled())std::cout<<"[ECON_SHIFT] f="<<info.GameFrame<<" farmer="<<f.SN
                <<" from="<<role<<" to=3 target="<<gold<<std::endl;
            break;
        }
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

    bool farmOrdered = resumeFarmConstruction(info);
    if(!farmOrdered)farmOrdered=rebalanceFood(info,foodCnt,woodCnt,goldCnt,targetFood,targetWood,targetGold);
    const int farmWoodGate = std::max((info.Meat < 150 ? 80 : FARM_WOOD_GATE),
        (bronze && m_researchCount[BUILDING_RANGE_UPGRADE_COMPOSITE_BOW] == 0)
            ? BUILDING_RANGE_UPGRADE_COMPOSITE_BOW_WOOD + 75 : 0);
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
        if (f.SN == m_depotBuilderSN || m_farmBuildOrders.count(f.SN)) continue;
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
                    int wantR = (info.GameFrame > FRAME_WAVE2) ? ((int)info.farmers.size() / 2)
                        : (info.GameFrame > FRAME_WAVE2 - 4500 ? 6 : 3);
                    if (wantR < FARM_MIN_COUNT) wantR = FARM_MIN_COUNT;
                    if (wantR > FARM_MAX_COUNT) wantR = FARM_MAX_COUNT;
                    if (!farmOrdered && farmSites(info) < wantR
                        && info.Wood >= farmWoodGate) {
                        int xp, yp;
                        if (findFarmSite(info, f, xp, yp)) {
                            farmBuild(this,info,f,xp,yp);
                            farmOrdered = true;
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
                econAction(this,info,f.SN,ksn);
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
                econAction(this,info,f.SN,gsn);
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
                // 浆果补位同样遵守已有的采集安全/不可达检查，不把空闲农民派往敌营。
                if (isBadTarget(r.SN, info.GameFrame) || !resSafe(info, r)) continue;
                int c = targetCount[r.SN];
                if (c < bestCnt) { bestCnt = c; bestSn = r.SN; }
            }
            if (bestSn >= 0) {
                econAction(this,info,f.SN,bestSn);
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
                econAction(this,info,f.SN,sn);
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
                econAction(this,info,f.SN,sn);
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
            // 已经花木材建成的空田先使用，不为远处/不可达猎物长期闲置。
            const int existingFarm=wave2Farm?findNearestFarm(info,f.SN):-1;
            if(existingFarm>=0){
                econAction(this,info,f.SN,existingFarm);m_issued.insert(f.SN);m_farmTaken.insert(existingFarm);
                m_orderTarget[f.SN]=existingFarm;m_orderFrame[f.SN]=info.GameFrame;
                m_orderX[f.SN]=f.DR;m_orderY[f.SN]=f.UR;
                m_foodGatherers.insert(f.SN);m_role[f.SN]=5;++foodCnt;continue;
            }
            int sn = findNearestHunt(info, f.SN);
            if (sn >= 0) {
                econAction(this,info,f.SN,sn);
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
                    if (fb.Cnt <= 0 || isBadTarget(fb.SN,info.GameFrame)) continue;
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
                    econAction(this,info,f.SN,farmSN);
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
                                    ? ((int)info.farmers.size() / 2)
                                    : (info.GameFrame > FRAME_WAVE2 - 4500 ? 6 : 3);
                    if (farmWantV < FARM_MIN_COUNT) farmWantV = FARM_MIN_COUNT;
                    if (farmWantV > FARM_MAX_COUNT) farmWantV = FARM_MAX_COUNT;
                    if (!farmOrdered && farmSites(info) < farmWantV
                        && (!mapFoodLeft(info) || info.Meat < 200)
                        && info.Wood >= farmWoodGate) {
                        int xf, yf;
                        if (findFarmSite(info, f, xf, yf)) {
                            farmBuild(this,info,f,xf,yf);
                            farmOrdered = true;
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
                econAction(this,info,f.SN,sn);
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
            econAction(this,info,f.SN,sn);
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
        if (b.Type != BUILDING_FARM || b.Percent < 100 || b.Cnt<=0 || m_farmTaken.count(b.SN)
            || isBadTarget(b.SN,info.GameFrame)) continue;
        bool occupied=false;
        for(const auto& worker:info.farmers)if(worker.SN!=farmerSN && worker.WorkObjectSN==b.SN
            &&worker.NowState!=HUMAN_STATE_IDLE)occupied=true;
        if(occupied)continue;
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
    const tagFarmer* farmer=nullptr;
    for(const auto& f:info.farmers)if(f.SN==farmerSN){farmer=&f;break;}
    if(!farmer)return -1;
    // 前三波保留已验证的打猎顺序；后期优先近处可用目标，避免长途空转。
    const bool lateHunt=info.GameFrame>=FRAME_WAVE3;
    const double huntRadius=(info.civilizationStage>=CIVILIZATION_BRONZEAGE?20.0:25.0)*BLOCKSIDELENGTH;
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
        if(lateHunt && isBadTarget(r.SN,info.GameFrame))continue;
        if(lateHunt && calDistance(farmer->DR,farmer->UR,r.DR,r.UR)>huntRadius)continue;
        if (!resSafe(info, r)) continue;                 // ★离敌营太近/离基地太远的猎物不碰
        if (r.Blood <= 0) corpses.push_back(r.SN);
        else alives.push_back(r.SN);
    }

    // 铜器后的补兵需要连续供粮；现有安全尸体有空位时直接采集，减少新狩猎等待。
    if(info.civilizationStage>=CIVILIZATION_BRONZEAGE){
        int best=-1,bestCount=1000000;double bestDistance=1e18;
        for(const auto& r:info.resources){
            if(std::find(corpses.begin(),corpses.end(),r.SN)==corpses.end()
                || isBadTarget(r.SN,info.GameFrame))continue;
            const int cap=r.Type==RESOURCE_ELEPHANT?3:(r.Type==RESOURCE_GAZELLE?2:1);
            const int users=cnt[r.SN];
            if(users>=cap)continue;
            const double distance=calDistance(farmer->DR,farmer->UR,r.DR,r.UR);
            if(users<bestCount || (users==bestCount && distance<bestDistance)){
                best=r.SN;bestCount=users;bestDistance=distance;
            }
        }
        if(best>=0)return best;
    }

    // ① 优先羚羊/狮子（安全猎物）：两两一组，猎人最少的优先
    //    （大象不在这一轮——羚羊没打完就不打大象）
    int safeBestSn = -1;
    int safeBestCnt = 1e9; double safeBestDist=1e18;
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
        const int rank=lateHunt?(c==1?0:1):c;
        const double distance=calDistance(farmer->DR,farmer->UR,rr->DR,rr->UR);
        if(rank<safeBestCnt || (lateHunt && rank==safeBestCnt && distance<safeBestDist)){
            safeBestCnt=rank;safeBestDist=distance;safeBestSn=sn;
        }
    }
    if (safeBestSn >= 0) return safeBestSn;

    // ② 羚羊/狮子都满员或没有了 → 才考虑大象
    //    大象必须多人集火：至少 3 人（含已在打/前往的）才安全；不足 3 人不开打（防送死）
    //    已有人开打（c>=1）→ 补员到最多 4 人；没人打（c==0）且空闲农民 <3 → 也不开
    if (!alives.empty()) {
        int eleBestSn = -1;
        int eleBestCnt = 1e9; double eleBestDist=1e18;
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
            const double distance=calDistance(farmer->DR,farmer->UR,rr->DR,rr->UR);
            if(c<eleBestCnt || (lateHunt && c==eleBestCnt && distance<eleBestDist)){
                eleBestCnt=c;eleBestDist=distance;eleBestSn=sn;
            }
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
        int bestCnt = 1e9; double bestDist=1e18;
        for (const tagResource& r : info.resources) {
            if (r.Blood > 0 || r.Cnt <= 0) continue;
            if(std::find(corpses.begin(),corpses.end(),r.SN)==corpses.end())continue;
            if (r.Type != RESOURCE_GAZELLE && r.Type != RESOURCE_ELEPHANT && r.Type != RESOURCE_LION) continue;
            int c = cnt[r.SN];
            int cap = (r.Type == RESOURCE_ELEPHANT) ? 3 : (r.Type == RESOURCE_GAZELLE ? 2 : 1);
            if (c >= cap) continue;
            const double distance=calDistance(farmer->DR,farmer->UR,r.DR,r.UR);
            if(c<bestCnt || (lateHunt && c==bestCnt && distance<bestDist)){
                bestCnt=c;bestDist=distance;bestSn=r.SN;
            }
        }
        if (bestSn >= 0) return bestSn;
    }
    return -1;   // 没有可安全打的猎物 → 农民去砍树/种田，不硬塞
}

static double goldDeliveryDistance(const tagInfo& info,const tagResource& mine)
{
    double nearest=1e18;
    const double x=mine.DR/BLOCKSIDELENGTH,y=mine.UR/BLOCKSIDELENGTH;
    for(const auto& b:info.buildings){
        if(b.Percent<100 || b.Blood<=0 || (b.Type!=BUILDING_STOCK && b.Type!=BUILDING_CENTER))continue;
        const double half=BUILD_SIZE[b.Type%BUILDING_TYPE_MAXNUM]*0.5;
        const double dx=std::max(0.0,std::fabs(x-b.BlockDR-half)-half);
        const double dy=std::max(0.0,std::fabs(y-b.BlockUR-half)-half);
        nearest=std::min(nearest,dx+dy);
    }
    return nearest;
}
static bool goldMineHasAccess(const tagResource& mine)
{
    for(int dx=-1;dx<=2;++dx)for(int dy=-1;dy<=2;++dy){
        if(dx>=0&&dx<2&&dy>=0&&dy<2)continue;
        const int x=mine.BlockDR+dx,y=mine.BlockUR+dy;
        if(x>=1&&y>=1&&x<99&&y<99 && (m_map[x][y]==0||m_map[x][y]==200))return true;
    }
    return false;
}
// 黄金同时考虑返仓距离和拥挤；其他资源保留原来的距离选择。
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
        if(type==RESOURCE_GOLD){
            if(!goldMineHasAccess(r))continue;
            int users=0;
            for(const auto& worker:info.farmers)if(worker.SN!=farmerSN
                && worker.NowState!=HUMAN_STATE_IDLE && worker.WorkObjectSN==r.SN)++users;
            d+=(goldDeliveryDistance(info,r)*3+users*3)*BLOCKSIDELENGTH;
        }
        if (d < best-1e-6 || (std::fabs(d-best)<=1e-6 && (sn<0 || r.SN<sn))) { best = d; sn = r.SN; }
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
static int queuedPopulation(const tagInfo& info);
static int pendingFrontPopulation(const tagInfo& info);
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
    if (bronzeNow && countArmy(info, AT_COMPOSITE_BOWMAN) < 6) needGoldFarmers = false;
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
            && (!bronzeNow || info.Human_MaxNum-info.Human_Num
                > queuedPopulation(info)+pendingFrontPopulation(info))
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
// 基地建筑串行由一位专职建造者负责；阵亡、不可达或长期无进度时换人接替。
//   实测问题：并行建造（抽调第二个空闲农民 + 靶场紧急抽调伐木工）导致
//     ① 开局有 2 个人在建房屋（伐木只剩 2 人）
//     ② 新生成的农民刚出生（IDLE）就被抓去建市场 → 不去采果/伐木
//   现在：建造者正在忙 → 本帧跳过，等他建完当前建筑再接下一条（串行建造）
// ============================================================
// 基地工地须有建造者当前能到达的施工边缘；地基生成后占地也必须绕开。
static bool baseConstructionReachable(const tagFarmer& farmer,int bx,int by,int size)
{
    const int sx=int(farmer.DR/BLOCKSIDELENGTH),sy=int(farmer.UR/BLOCKSIDELENGTH);
    if(sx<0||sy<0||sx>=100||sy>=100||size<=0)return false;
    auto open=[&](int x,int y){return x>=0&&y>=0&&x<100&&y<100
        &&!(x>=bx&&x<bx+size&&y>=by&&y<by+size)
        &&(m_map[x][y]==0||(x==sx&&y==sy&&m_map[x][y]==200));};
    if(!open(sx,sy))return false;
    bool seen[10000]={};int queue[10000],head=0,tail=0;seen[sx*100+sy]=true;queue[tail++]=sx*100+sy;
    while(head<tail){
        const int at=queue[head++],x=at/100,y=at%100;
        if(((x==bx-1||x==bx+size)&&y>=by&&y<by+size)
            ||((y==by-1||y==by+size)&&x>=bx&&x<bx+size))return true;
        for(int dx=-1;dx<=1;++dx)for(int dy=-1;dy<=1;++dy){
            const int nx=x+dx,ny=y+dy;
            if((!dx&&!dy)||!open(nx,ny)||(dx&&dy&&(!open(x+dx,y)||!open(x,y+dy))))continue;
            const int next=nx*100+ny;if(!seen[next]){seen[next]=true;queue[tail++]=next;}
        }
    }
    return false;
}
struct BaseBuildOrder {
    int actor=-1,id=-1,frame=-1,progressFrame=-1,type=-1,x=0,y=0,percent=0;
    double px=0,py=0;
};
static BaseBuildOrder m_baseBuildOrder;
static int baseReplacement(const tagInfo& info,int old,int type,int x,int y)
{
    int best=-1;double distance=1e18;
    for(const auto& f:info.farmers){
        if(f.SN==old||f.SN==m_depotBuilderSN||f.FarmerSort!=FARMERTYPE_FARMER
            ||f.Blood<=0||f.Resource>0||m_issued.count(f.SN))continue;
        bool busy=false;for(const auto& b:info.buildings)
            if(b.SN==f.WorkObjectSN&&b.Percent<100)busy=true;
        if(busy||!baseConstructionReachable(f,x,y,BUILD_SIZE[type%BUILDING_TYPE_MAXNUM]))continue;
        const double d=std::hypot(f.DR/BLOCKSIDELENGTH-x,f.UR/BLOCKSIDELENGTH-y);
        if(d<distance){best=f.SN;distance=d;}
    }
    return best;
}
static void baseBuild(UsrAI* self,const tagInfo& info,int actor,int type,int x,int y)
{
    const tagFarmer* worker=nullptr;for(const auto& f:info.farmers)if(f.SN==actor){worker=&f;break;}
    if(!worker||!baseConstructionReachable(*worker,x,y,BUILD_SIZE[type%BUILDING_TYPE_MAXNUM])){
        const int replacement=baseReplacement(info,actor,type,x,y);
        if(replacement<0){m_badBuildSite[x*100+y]=info.GameFrame;return;}
        if(aiTraceEnabled())std::cout<<"[BASE_HANDOFF] f="<<info.GameFrame<<" old="<<actor<<" new="<<replacement<<" reason=unreachable"<<std::endl;
        actor=m_builderSN=replacement;m_issued.insert(actor);
    }
    m_baseBuildOrder=BaseBuildOrder{};
    auto& o=m_baseBuildOrder;
    o.actor=actor;o.type=type;o.x=x;o.y=y;o.frame=o.progressFrame=info.GameFrame;
    for(const auto& f:info.farmers)if(f.SN==actor){o.px=f.DR;o.py=f.UR;break;}
    o.id=issueHumanBuild(self,info,actor,type,x,y);
    if(o.id<0)o=BaseBuildOrder{};
}
static bool baseBuilderAvailable(const tagInfo& info,const tagFarmer& f)
{
    auto& o=m_baseBuildOrder;
    if(o.actor!=f.SN)o=BaseBuildOrder{};
    // 快照可能仍指向未完工工地，但原指令记录已丢失；也要纳入进度检查。
    if(o.actor<0){
        for(const auto& b:info.buildings)if(b.SN==f.WorkObjectSN&&b.Percent<100){
            o.actor=f.SN;o.type=b.Type;o.x=b.BlockDR;o.y=b.BlockUR;o.percent=b.Percent;
            o.frame=o.progressFrame=info.GameFrame;o.px=f.DR;o.py=f.UR;
            break;
        }
    }
    if(o.actor==f.SN){
        auto ret=info.ins_ret.find(o.id);
        if(ret!=info.ins_ret.end()&&ret->second!=ACTION_SUCCESS)o=BaseBuildOrder{};
        else {
            bool done=false;
            for(const auto& b:info.buildings)if(b.Type==o.type&&b.BlockDR==o.x&&b.BlockUR==o.y){
                done=b.Percent>=100;
                if(b.Percent>o.percent){o.percent=b.Percent;o.progressFrame=info.GameFrame;}
                break;
            }
            if(done)o=BaseBuildOrder{};
            else {
                if(std::hypot(f.DR-o.px,f.UR-o.py)>BLOCKSIDELENGTH/4.0){
                    o.px=f.DR;o.py=f.UR;o.progressFrame=info.GameFrame;
                }
                if(info.GameFrame-o.progressFrame<250)return false;
                const int replacement=baseReplacement(info,f.SN,o.type,o.x,o.y);
                if(replacement>=0){
                    m_builderSN=replacement;
                    if(aiTraceEnabled())std::cout<<"[BASE_HANDOFF] f="<<info.GameFrame<<" old="<<f.SN<<" new="<<replacement<<" reason=no_progress"<<std::endl;
                }
                o=BaseBuildOrder{}; // 长时间无施工/位移进度允许重新派活。
                return true;
            }
        }
    }
    // 接替的村民可能仍在采集；专职建造职责应主动接管，不能永久等IDLE。
    return true;
}
// 先完成可到达的在建基地建筑，避免住房地基占满目标却永远没有人口。
static bool baseResumeConstruction(UsrAI* self,const tagInfo& info,int actor,int previous=-1)
{
    const tagFarmer* worker=nullptr;for(const auto& f:info.farmers)if(f.SN==actor){worker=&f;break;}
    if(!worker)return false;
    const tagBuilding* target=nullptr;double best=1e18;
    for(const auto& b:info.buildings){
        if(b.Percent>=100||b.Type==BUILDING_FARM)continue;
        bool assigned=false;for(const auto& f:info.farmers)if(f.SN!=actor&&f.SN!=previous&&f.Blood>0
            &&f.NowState!=HUMAN_STATE_IDLE&&f.WorkObjectSN==b.SN)assigned=true;
        const double distance=std::hypot(worker->DR/BLOCKSIDELENGTH-b.BlockDR,
            worker->UR/BLOCKSIDELENGTH-b.BlockUR);
        if(!assigned&&distance<best
            &&baseConstructionReachable(*worker,b.BlockDR,b.BlockUR,BUILD_SIZE[b.Type%BUILDING_TYPE_MAXNUM])){
            target=&b;best=distance;
        }
    }
    if(!target)return false;
    m_baseBuildOrder=BaseBuildOrder{};auto& o=m_baseBuildOrder;
    o.actor=actor;o.type=target->Type;o.x=target->BlockDR;o.y=target->BlockUR;
    o.frame=o.progressFrame=info.GameFrame;o.percent=target->Percent;o.px=worker->DR;o.py=worker->UR;
    o.id=self->HumanAction(actor,target->SN);m_issued.insert(actor);
    if(aiTraceEnabled())std::cout<<"[BASE_RESUME] f="<<info.GameFrame<<" farmer="<<actor
        <<" building="<<target->SN<<" type="<<target->Type<<" percent="<<target->Percent<<std::endl;
    return true;
}
void UsrAI::buildBuildings(const tagInfo& info)
{
    // 找专职建造者：只有他"空闲且本帧没被下令"时才派活
    int builder = -1;
    const int previous=m_builderSN;
    if (m_builderSN >= 0) {
        for (const tagFarmer& f : info.farmers) {
            if (f.SN != m_builderSN) continue;
            if (!m_issued.count(f.SN) && baseBuilderAvailable(info,f)) builder = m_builderSN;
            break;
        }
    }
    if (builder == -1) return;      // 没有建造者/他正在建 → 绝不占用采集农民
    if(baseResumeConstruction(this,info,builder,previous))return;

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
        // 正在采的矿返仓超过8格时，先缩短运输；不再等六弓和全部住房都完成。
        int stocks=0,goldWorkers=0;
        bool stockBuilding=false;
        for(const auto& b:info.buildings)if(b.Type==BUILDING_STOCK){++stocks;stockBuilding|=b.Percent<100;}
        for(const auto& f:info.farmers)if(f.NowState!=HUMAN_STATE_IDLE)
            for(const auto& r:info.resources)if(r.SN==f.WorkObjectSN && r.Type==RESOURCE_GOLD && r.Cnt>0)++goldWorkers;
        const int goldDepotGate=BUILD_STOCK_WOOD+(m_researchCount[BUILDING_RANGE_UPGRADE_COMPOSITE_BOW]==0
            ?BUILDING_RANGE_UPGRADE_COMPOSITE_BOW_WOOD:0);
        if(info.civilizationStage>=CIVILIZATION_BRONZEAGE && countBuilding(info,BUILDING_RANGE)>0
            && stocks<3 && !stockBuilding && goldWorkers>=2 && info.Wood>=goldDepotGate
            && info.Human_Num<info.Human_MaxNum-2){
            const tagResource* mine=nullptr;double best=0;
            for(const auto& r:info.resources){
                if(r.Type!=RESOURCE_GOLD || r.Cnt<=0 || isBadTarget(r.SN,info.GameFrame)
                    || !resSafe(info,r) || !goldMineHasAccess(r))continue;
                int users=0;for(const auto& f:info.farmers)if(f.WorkObjectSN==r.SN
                    && f.NowState!=HUMAN_STATE_IDLE)++users;
                const double delivery=goldDeliveryDistance(info,r);
                const double score=users*delivery;
                if(delivery>8 && users>0 && (score>best || (score==best && mine && r.SN<mine->SN))){
                    best=score;mine=&r;
                }
            }
            int x,y;
            if(mine && findBuildBlockNear(info,x,y,3,3,mine->BlockDR,mine->BlockUR,6)){
                const double next=std::max(0.0,std::fabs(mine->DR/BLOCKSIDELENGTH-x-1.5)-1.5)
                    +std::max(0.0,std::fabs(mine->UR/BLOCKSIDELENGTH-y-1.5)-1.5);
                if(next+4<goldDeliveryDistance(info,*mine)){
                    baseBuild(this,info,builder,BUILDING_STOCK,x,y);m_issued.insert(builder);return;
                }
            }
        }
        int homes = countBuilding(info, BUILDING_HOME);
        int houseTarget = (countBuilding(info, BUILDING_RANGE) > 0) ? 6 : 4;
        bool bronzeNow2 = (info.civilizationStage >= CIVILIZATION_BRONZEAGE);
        if (bronzeNow2 && countBuilding(info, BUILDING_RANGE) > 0) houseTarget = 8;
        // 【用户要求】原来的 4/6/8 阶梯逻辑**不动**；8 座达成后**不等人口告急**，
        //   直接把目标抬到 12 → 一口气从 8 座建到 12 座（≈50 人口，正好到上限）。
        //   原来这里是"人口≥上限-2 就把目标设为 现有房数+1"，每次只加 1 座、还得等人口再满
        //   —— 现在按"房≥8"直接触发，不再依赖人口。
        const bool protectArmy = bronzeNow2 && countArmy(info, AT_COMPOSITE_BOWMAN) < 6;
        if (bronzeNow2 && !protectArmy && countBuilding(info, BUILDING_RANGE) < 2
            && info.Wood >= BUILD_RANGE_WOOD && info.Human_Num < info.Human_MaxNum - 2) {
            int x, y;
            if (findBuildBlock(info, x, y, 3, 3, m_centerX, m_centerY)) {
                baseBuild(this,info,builder, BUILDING_RANGE, x, y); m_issued.insert(builder); return;
            }
        }
        int houseNormalTarget = houseTarget;      // 正常目标(4/6/8)，供马厩当"住房已完成"的前提
        if (bronzeNow2 && countBuilding(info, BUILDING_RANGE) > 0 && homes >= 8) {
            houseTarget = 12;
        }
        if (protectArmy && info.Human_Num < info.Human_MaxNum - 3) houseTarget = homes;
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
        if (!built && !protectArmy && info.civilizationStage >= CIVILIZATION_BRONZEAGE   // 前提：铜器后
            && countBuilding(info, BUILDING_ARMYCAMP) > 0     // 引擎前置：马厩←兵营
            && countBuilding(info, BUILDING_RANGE) > 0        // 靶场已建
            && homes >= houseNormalTarget                     // 住房目标(8/6)已完成
            && countBuilding(info, BUILDING_STABLE) == 0 && info.Wood >= BUILD_STABLE_WOOD) {
            int x, y;
            if (findBuildBlock(info, x, y, 3, 3)) {
                baseBuild(this,info,builder, BUILDING_STABLE, x, y);
                m_issued.insert(builder);
                return;                     // 本帧只下这一条令（搬到高位后必须立刻结束）
            }
        }

        // ===== 2) 住房：目标 4→6→8（正常），8 座达成后一口气补到 12（不等人口告急）=====
        if (countBuilding(info, BUILDING_HOME) < houseTarget && info.Wood >= BUILD_HOUSE_WOOD) {
            int x, y;
            if (findBuildBlock(info, x, y, 2, 2)) {
                baseBuild(this,info,builder, BUILDING_HOME, x, y);
                m_issued.insert(builder);
                return;                     // 本帧只下这一条令（搬到高位后必须立刻结束）
            }
        }

        // ===== 4) 学院（从原 13 号位提前到这里；加"马厩已建"的引擎前置）=====
        if (!built && !protectArmy && info.civilizationStage >= CIVILIZATION_BRONZEAGE
            && countBuilding(info, BUILDING_STABLE) > 0          // 引擎前置：学院←马厩
            && countBuilding(info, BUILDING_COLLAGE) == 0 && info.Wood >= BUILD_COLLAGE_WOOD) {
            int x, y;
            if (findBuildBlock(info, x, y, 3, 3)) {
                baseBuild(this,info,builder, BUILDING_COLLAGE, x, y);
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
                    baseBuild(this,info,builder, BUILDING_ARROWTOWER, tx2, ty2);
                    m_issued.insert(builder);
                    return;                     // 本帧只下这一条建造令
                }
            } else if (countBuilding(info, BUILDING_GRANARY) == 0
                       && info.Wood >= BUILD_GRANARY_WOOD) {
                // 还没有谷仓（箭塔科技没地方研发）→ 先补一座谷仓
                int gx2, gy2;
                if (findBuildBlock(info, gx2, gy2, 3, 3)) {
                    baseBuild(this,info,builder, BUILDING_GRANARY, gx2, gy2);
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
                baseBuild(this,info,builder, BUILDING_MARKET, x, y);
                m_issued.insert(builder);
                built = true;
            }
        }
        // 4) 兵营（靶场前置）
        else if (countBuilding(info, BUILDING_MARKET) > 0
            && countBuilding(info, BUILDING_ARMYCAMP) == 0 && info.Wood >= BUILD_ARMYCAMP_WOOD) {
            int x, y;
            if (findBuildBlock(info, x, y, 3, 3)) {
                baseBuild(this,info,builder, BUILDING_ARMYCAMP, x, y);
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
                baseBuild(this,info,builder, BUILDING_RANGE, x, y);
                m_issued.insert(builder);
                built = true;
            }
        }
        // 【已上移·用户要求】原链里的"马厩"已提到优先级 1。
        // 【已上移·用户要求】原链里的"学院"已提到优先级 4。
        // 【已删除·用户要求】原链里的"农田"。农田一律由食物系农民自己建。
        // 【已上移·用户要求】原 15 号位的"金矿旁仓库"已提到优先级 3。
        // （羚羊堆仓库/浆果堆谷仓由采集者负责，见 buildResourceDepots）
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
            issueHumanBuild(this,info,m_depotBuilderSN,BUILDING_STOCK,x,y);
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
            issueHumanBuild(this,info,m_depotBuilderSN,BUILDING_GRANARY,x,y);
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
struct ResearchRequest { int action, building, frame; };
static std::map<int, ResearchRequest> m_pendingResearch;
static void issueResearch(UsrAI* self, const tagInfo& info, int building, int action)
{
    int id = self->BuildingAction(building, action);
    m_pendingResearch[id] = {action,building,info.GameFrame};
}
static void confirmResearch(const tagInfo& info)
{
    for (auto it=m_pendingResearch.begin();it!=m_pendingResearch.end();) {
        auto ret=info.ins_ret.find(it->first);
        if (ret!=info.ins_ret.end()) {
            if (ret->second!=0 && m_researchCount[it->second.action]>0) --m_researchCount[it->second.action];
            it=m_pendingResearch.erase(it);
        } else ++it;
    }
}

// 用同一张连通图识别被建筑/树林封在小空地里的兵，避免计入出征编制又永远等不到。
// 未知地形只用于“尚不能证明被封死”的乐观判断；实际行军仍走已知可达路点。
struct ArmyReachability { bool seen[10000]={}; int frame=-9999; };
static ArmyReachability m_armyReachability;
static bool armyTravelPossible(const tagInfo& info,const tagArmy& soldier)
{
    auto open=[](int x,int y){return x>=0&&y>=0&&x<std::min(100,int(MAP_L))
        &&y<std::min(100,int(MAP_U))&&(m_map[x][y]==0||m_map[x][y]==200||m_map[x][y]==-2);};
    auto& area=m_armyReachability;
    if(area.frame<0 || info.GameFrame<area.frame || info.GameFrame-area.frame>=50){
        area.frame=info.GameFrame;std::fill(area.seen,area.seen+10000,false);
        int x=m_centerX+4,y=m_centerY+4;
        for(const auto& a:info.armies)if(a.Sort==AT_PRIEST&&a.Blood>0){
            x=int(a.DR/BLOCKSIDELENGTH);y=int(a.UR/BLOCKSIDELENGTH);break;
        }
        if(!open(x,y)){
            bool found=false;
            for(int r=1;r<=3&&!found;++r)for(int dx=-r;dx<=r&&!found;++dx)
                for(int dy=-r;dy<=r&&!found;++dy)if(open(x+dx,y+dy)){x+=dx;y+=dy;found=true;}
            if(!found){area.frame=-9999;return true;}
        }
        int queue[10000],head=0,tail=0;queue[tail++]=x*100+y;area.seen[x*100+y]=true;
        while(head<tail){
            const int at=queue[head++],cx=at/100,cy=at%100;
            for(int dx=-1;dx<=1;++dx)for(int dy=-1;dy<=1;++dy){
                const int nx=cx+dx,ny=cy+dy;
                if((!dx&&!dy)||!open(nx,ny)||(dx&&dy&&(!open(cx+dx,cy)||!open(cx,cy+dy))))continue;
                const int index=nx*100+ny;
                if(!area.seen[index]){area.seen[index]=true;queue[tail++]=index;}
            }
        }
    }
    const int x=int(soldier.DR/BLOCKSIDELENGTH),y=int(soldier.UR/BLOCKSIDELENGTH);
    // 刚出生尚位于生产建筑占地时，先由出营逻辑处理，不能误当永久隔绝。
    return !open(x,y)||area.seen[x*100+y];
}
struct ArmyMix { int composite=0,frontline=0,healthyFrontline=0,queuedComposite=0,queuedFrontline=0; };
static const int COUNTER_COMPOSITE_TARGET=10;
static const int COUNTER_FRONT_TARGET=4;
static bool frontlineType(int sort)
{
    return sort==AT_CLUBMAN || sort==AT_SWORDSMAN || sort==AT_IMPROVED
        || sort==AT_CAVALRY || sort==AT_HOPLITE || sort==AT_CHARIOT || sort==AT_BROADSWORDSMAN;
}
static ArmyMix collectArmyMix(const tagInfo& info)
{
    ArmyMix mix;
    for(const auto& a:info.armies)if(a.Blood>0&&armyTravelPossible(info,a)){
        if(a.Sort==AT_COMPOSITE_BOWMAN)++mix.composite;
        if(frontlineType(a.Sort)){
            ++mix.frontline;
            if(a.MaxBlood>0 && a.Blood*2>=a.MaxBlood)++mix.healthyFrontline;
        }
    }
    for(const auto& b:info.buildings)if(b.Percent==100){
        if(b.Type==BUILDING_RANGE && b.Project==BUILDING_RANGE_CREATE_COMPOSITE_BOWMAN)++mix.queuedComposite;
        if((b.Type==BUILDING_ARMYCAMP && b.Project==BUILDING_ARMYCAMP_CREATE_BROADSWORD)
            ||(b.Type==BUILDING_STABLE && b.Project==BUILDING_STABLE_CREATE_CAVALRY)
            ||(b.Type==BUILDING_COLLAGE && b.Project==BUILDING_COLLAGE_CREATE_HOPLITE))++mix.queuedFrontline;
    }
    return mix;
}
static int frontlineGoal(const ArmyMix& mix)
{
    if(!m_atkOn && mix.composite>=10)return COUNTER_FRONT_TARGET;
    return std::min(5,std::max(2,mix.composite/4+1));
}
static int queuedPopulation(const tagInfo& info)
{
    const ArmyMix mix=collectArmyMix(info);
    int queued=mix.queuedComposite+mix.queuedFrontline;
    for(const auto& b:info.buildings)if(b.Percent==100 && b.Type==BUILDING_CENTER
        && b.Project==BUILDING_CENTER_CREATEFARMER)++queued;
    // 本帧刚发出的请求尚未写入快照；其他产线不能重复使用这个名额。
    for(const auto& b:info.buildings)if(b.Percent==100 && b.Project==ACT_NULL
        && m_issued.count(b.SN) && (b.Type==BUILDING_CENTER || b.Type==BUILDING_RANGE
            || b.Type==BUILDING_ARMYCAMP || b.Type==BUILDING_STABLE || b.Type==BUILDING_COLLAGE))++queued;
    return queued;
}
static int pendingFrontPopulation(const tagInfo& info)
{
    const ArmyMix mix=collectArmyMix(info);
    return info.civilizationStage>=CIVILIZATION_BRONZEAGE && mix.composite>=6
        ?std::max(0,frontlineGoal(mix)-mix.healthyFrontline-mix.queuedFrontline):0;
}
static bool counterArmyReady(const tagInfo& info)
{
    int composite=0,healthyFront=0;
    for(const auto& a:info.armies)if(a.Blood>0&&armyTravelPossible(info,a)){
        if(a.Sort==AT_COMPOSITE_BOWMAN)++composite;
        if(frontlineType(a.Sort)&&a.MaxBlood>0&&a.Blood*2>=a.MaxBlood)++healthyFront;
    }
    return composite>=COUNTER_COMPOSITE_TARGET && healthyFront>=COUNTER_FRONT_TARGET;
}
static bool frontlineBudgetNeeded(const tagInfo& info)
{
    if(info.civilizationStage<CIVILIZATION_BRONZEAGE)return false;
    const ArmyMix mix=collectArmyMix(info);
    return mix.composite>=6 && mix.healthyFrontline+mix.queuedFrontline<frontlineGoal(mix);
}

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
                issueResearch(this, info, b.SN, BUILDING_RANGE_UPGRADE_COMPOSITE_BOW);
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
        // 六名复合弓前，其他耗粮科技让路；箭塔科技仍允许研发。
        if (bronze && countArmy(info, AT_COMPOSITE_BOWMAN) < 6 && b.Type != BUILDING_GRANARY) continue;
        // 阵型缺前排时，非必要科技让粮金用于补兵；兵营科技仍可解锁便宜前排。
        if(bronze && frontlineBudgetNeeded(info) && b.Type!=BUILDING_GRANARY
            && b.Type!=BUILDING_ARMYCAMP)continue;
        switch (b.Type) {
        case BUILDING_GRANARY: {
            // 【用户要求·建完靶场立刻补第二座塔】必须研发箭塔科技（引擎硬前置：Development.cpp:768）
            //   只在"真要建塔"时才花这 50 食：靶场已建 + 塔不足 2 座 + 石头够一座塔(150)
            if (countBuilding(info, BUILDING_RANGE) > 0
                && countBuilding(info, BUILDING_ARROWTOWER) < 2
                && info.Stone >= BUILD_ARROWTOWER_STONE
                && m_researchCount[BUILDING_GRANARY_ARROWTOWER] == 0
                && info.Meat >= BUILDING_GRANARY_ARROWTOWER_FOOD) {
                issueResearch(this, info, b.SN, BUILDING_GRANARY_ARROWTOWER);
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
                issueResearch(this, info, b.SN, BUILDING_MARKET_WOOD_UPGRADE);
                m_issued.insert(b.SN);
                m_researchCount[BUILDING_MARKET_WOOD_UPGRADE]++;
                break;
            }
            // 【发育策略】采石科技停用（不挖石、不造塔/投石兵）→ 省 100 食 + 50 石
            // 【发育策略】车轮科技停用（不出四马战车/战车弓兵）→ 省 150 食 + 100 木
            if (bronze && m_researchCount[BUILDING_MARKET_GOLD_UPGRADE] == 0
                && info.Meat >= BUILDING_MARKET_GOLD_UPGRADE_FOOD
                && info.Wood >= BUILDING_MARKET_GOLD_UPGRADE_WOOD) {
                issueResearch(this, info, b.SN, BUILDING_MARKET_GOLD_UPGRADE);
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
                issueResearch(this, info, b.SN, BUILDING_STOCK_UPGRADE_USETOOL);
                m_issued.insert(b.SN);
                m_researchCount[BUILDING_STOCK_UPGRADE_USETOOL]++;
                break;
            }
            if (bronze && ut == 1 && info.Meat >= BUILDING_STOCK_UPGRADE_CLOSER_ATTACK_2_FOOD
                && info.Gold >= BUILDING_STOCK_UPGRADE_CLOSER_ATTACK_2_GOLD) {
                issueResearch(this, info, b.SN, BUILDING_STOCK_UPGRADE_USETOOL);
                m_issued.insert(b.SN);
                m_researchCount[BUILDING_STOCK_UPGRADE_USETOOL]++;
                break;
            }
            // 步兵护甲
            // 【3.0.7g 新策略】护甲科技让位：先保证主力兵种科技（阔剑/复合弓）解锁，食物紧张
            if (bronze && unitTechReady
                && m_researchCount[BUILDING_STOCK_UPGRADE_DEFENSE_INFANTRY] == 0
                && info.Meat >= BUILDING_STOCK_UPGRADE_DEFENSE_INFANTRY_FOOD) {
                issueResearch(this, info, b.SN, BUILDING_STOCK_UPGRADE_DEFENSE_INFANTRY);
                m_issued.insert(b.SN);
                m_researchCount[BUILDING_STOCK_UPGRADE_DEFENSE_INFANTRY]++;
                break;
            }
            // 弓兵护甲
            if (bronze && unitTechReady
                && m_researchCount[BUILDING_STOCK_UPGRADE_DEFENSE_ARCHER] == 0
                && info.Meat >= BUILDING_STOCK_UPGRADE_DEFENSE_ARCHER_FOOD) {
                issueResearch(this, info, b.SN, BUILDING_STOCK_UPGRADE_DEFENSE_ARCHER);
                m_issued.insert(b.SN);
                m_researchCount[BUILDING_STOCK_UPGRADE_DEFENSE_ARCHER]++;
                break;
            }
            // 骑兵护甲
            if (bronze && unitTechReady
                && m_researchCount[BUILDING_STOCK_UPGRADE_DEFENSE_RIDER] == 0
                && info.Meat >= BUILDING_STOCK_UPGRADE_DEFENSE_RIDER_FOOD) {
                issueResearch(this, info, b.SN, BUILDING_STOCK_UPGRADE_DEFENSE_RIDER);
                m_issued.insert(b.SN);
                m_researchCount[BUILDING_STOCK_UPGRADE_DEFENSE_RIDER]++;
                break;
            }
            // 青铜盾（铜器）
            if (bronze && m_researchCount[BUILDING_STOCK_UPGRADE_MISSILE_DEFENSE_INFANTRY] == 0
                && info.Meat >= BUILDING_STOCK_UPGRADE_MISSILE_DEFENSE_INFANTRY_FOOD
                && info.Gold >= BUILDING_STOCK_UPGRADE_MISSILE_DEFENSE_INFANTRY_GOLD) {
                issueResearch(this, info, b.SN, BUILDING_STOCK_UPGRADE_MISSILE_DEFENSE_INFANTRY);
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
                issueResearch(this, info, b.SN, BUILDING_ARMYCAMP_UPGRADE_BROADSWORD);
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
                issueResearch(this, info, b.SN, BUILDING_RANGE_UPGRADE_COMPOSITE_BOW);
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
static bool defenseRanged(const tagArmy& a);
static int m_armyReserveTraceFrame=-9999;

void UsrAI::trainArmy(const tagInfo& info)
{
    if (info.Human_Num >= info.Human_MaxNum) return;
    const bool bronze = info.civilizationStage >= CIVILIZATION_BRONZEAGE;
    const bool unlocked = m_researchCount[BUILDING_RANGE_UPGRADE_COMPOSITE_BOW] > 0;
    // 解锁前保存 180 食与 100 木；否则补农、普通弓、骑兵会把科技永远饿死。
    if (bronze && !unlocked) return;
    const ArmyMix mix=collectArmyMix(info);
    // 训练队列尚未计入当前人口，必须预留出生名额；否则弓兵会占满前排出兵所需的人口。
    if(info.Human_MaxNum-info.Human_Num<=queuedPopulation(info))return;
    // 初始六复合弓形成后，先保证2～5名健康前排；排队中的前排也占预算。
    // 只使用已完成且空闲的生产建筑，没有可用前排产线时继续补弓。
    if(bronze && mix.composite>=6 && mix.healthyFrontline+mix.queuedFrontline<frontlineGoal(mix)){
        const tagBuilding* producer=nullptr;
        int action=-1,food=0,gold=0,priority=99;
        for(const auto& b:info.buildings){
            if(b.Percent<100 || b.Project!=ACT_NULL || m_issued.count(b.SN))continue;
            int act=-1,needFood=0,needGold=0,rank=99;
            if(b.Type==BUILDING_COLLAGE){act=BUILDING_COLLAGE_CREATE_HOPLITE;
                needFood=BUILDING_COLLAGE_CREATE_HOPLITE_FOOD;needGold=BUILDING_COLLAGE_CREATE_HOPLITE_GOLD;rank=0;}
            else if(b.Type==BUILDING_ARMYCAMP && m_researchCount[BUILDING_ARMYCAMP_UPGRADE_BROADSWORD]>0){
                act=BUILDING_ARMYCAMP_CREATE_BROADSWORD;needFood=BUILDING_ARMYCAMP_CREATE_BROADSWORD_FOOD;
                needGold=BUILDING_ARMYCAMP_CREATE_BROADSWORD_GOLD;rank=1;}
            else if(b.Type==BUILDING_STABLE){act=BUILDING_STABLE_CREATE_CAVALRY;
                needFood=BUILDING_STABLE_CREATE_CAVALRY_FOOD;needGold=BUILDING_STABLE_CREATE_CAVALRY_GOLD;rank=2;}
            if(act<0)continue;
            // 有能立即训练的产线先用；全部缺资源时为选中的前排留预算。
            if(info.Meat<needFood || info.Gold<needGold)rank+=10;
            if(rank<priority || (rank==priority && (!producer || b.SN<producer->SN))){
                producer=&b;action=act;food=needFood;gold=needGold;priority=rank;
            }
        }
        if(producer){
            if(info.Meat>=food && info.Gold>=gold){
                BuildingAction(producer->SN,action);m_issued.insert(producer->SN);
                if(aiTraceEnabled())std::cout<<"[ARMY_FRONT] f="<<info.GameFrame<<" action="<<action
                    <<" comp="<<mix.composite<<" healthy="<<mix.healthyFrontline
                    <<" queued="<<mix.queuedFrontline<<" goal="<<frontlineGoal(mix)<<std::endl;
            }else if(aiTraceEnabled() && info.GameFrame-m_armyReserveTraceFrame>=250){
                std::cout<<"[ARMY_RESERVE] f="<<info.GameFrame<<" need="<<food<<","<<gold
                    <<" stock="<<info.Meat<<","<<info.Gold<<std::endl;
                m_armyReserveTraceFrame=info.GameFrame;
            }
            return;
        }
        // 没有已解锁的前排产线时，先攒兵营解锁资源；否则持续产弓会饿死这段生产链。
        bool frontlineLine=false,lockedCamp=false;
        for(const auto& b:info.buildings)if(b.Percent==100){
            if(b.Type==BUILDING_COLLAGE || b.Type==BUILDING_STABLE
                ||(b.Type==BUILDING_ARMYCAMP&&m_researchCount[BUILDING_ARMYCAMP_UPGRADE_BROADSWORD]>0))frontlineLine=true;
            if(b.Type==BUILDING_ARMYCAMP&&m_researchCount[BUILDING_ARMYCAMP_UPGRADE_BROADSWORD]==0)lockedCamp=true;
        }
        if(lockedCamp&&!frontlineLine)return;
    }
    // 十弓是出征下限，不是生产上限；同时给尚未排队的前排保留人口。
    if(info.Human_MaxNum-info.Human_Num<=queuedPopulation(info)+pendingFrontPopulation(info))return;
    // 单帧只选一个生产请求，避免快照资源重复花费。
    for (const auto& b : info.buildings) {
        if (b.Type != BUILDING_RANGE || b.Percent < 100 || b.Project != ACT_NULL || m_issued.count(b.SN)) continue;
        // 升铜过程中留存食物给复合弓科技，避免普通弓队列拖住解锁。
        if (bronze && info.Meat >= BUILDING_RANGE_CREATE_COMPOSITE_BOWMAN_FOOD && info.Gold >= 20) {
            BuildingAction(b.SN,BUILDING_RANGE_CREATE_COMPOSITE_BOWMAN); m_issued.insert(b.SN); return;
        }
    }
    if (!bronze || mix.composite < 6
        || mix.healthyFrontline+mix.queuedFrontline>=frontlineGoal(mix)) return;
    for (const auto& b : info.buildings) {
        if (b.Percent < 100 || b.Project != ACT_NULL || m_issued.count(b.SN)) continue;
        int action = -1;
        if (b.Type == BUILDING_ARMYCAMP && m_researchCount[BUILDING_ARMYCAMP_UPGRADE_BROADSWORD] > 0
            && info.Meat >= BUILDING_ARMYCAMP_CREATE_BROADSWORD_FOOD && info.Gold >= 15)
            action = BUILDING_ARMYCAMP_CREATE_BROADSWORD;
        else if (b.Type == BUILDING_STABLE && info.Meat >= BUILDING_STABLE_CREATE_CAVALRY_FOOD && info.Gold >= 80)
            action = BUILDING_STABLE_CREATE_CAVALRY;
        else if (b.Type == BUILDING_COLLAGE && info.Meat >= BUILDING_COLLAGE_CREATE_HOPLITE_FOOD && info.Gold >= 40)
            action = BUILDING_COLLAGE_CREATE_HOPLITE;
        if (action >= 0) { BuildingAction(b.SN,action); m_issued.insert(b.SN); return; }
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
                        issueHumanBuild(this,info,f.SN,BUILDING_ARROWTOWER,bx,by);
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
struct TowerAttackOrder { int target,id; };
static std::map<int,TowerAttackOrder> m_towerOrders;
static bool defenseRanged(const tagArmy& a)
{
    return a.Sort == AT_BOWMAN || a.Sort == AT_COMPOSITE_BOWMAN
        || a.Sort == AT_CHARIOT_ARCHER || a.Sort == AT_SLINGER;
}
static int defenseSelfConvertTarget(const tagInfo& info,const tagArmy& priest,int preferred)
{
    // WorkObjectSN是追击目标，不代表已经打到祭司；只转当前射程内的单位。
    const double range=(double(DIS_PRIEST)-1.0)*BLOCKSIDELENGTH;
    int target=-1,rank=-1;double nearest=1e18;
    for(const auto& e:info.enemy_armies){
        if(e.Blood<=0)continue;
        const double d=std::hypot(e.DR-priest.DR,e.UR-priest.UR);
        if(d>range)continue;
        if(e.SN==preferred)return e.SN;
        const int priority=(e.WorkObjectSN==priest.SN?10:0)
            +(e.Sort==AT_CHARIOT_ARCHER?2:(defenseRanged(e)?1:0));
        if(priority>rank || (priority==rank && (d<nearest || (d==nearest && e.SN<target)))){
            rank=priority;nearest=d;target=e.SN;
        }
    }
    return target;
}
static bool defenseConversionTooFar(const tagInfo& info,const tagArmy& priest)
{
    if(priest.ConvertCooldown>0)return false;
    for(const auto& e:info.enemy_armies)if(e.Blood>0&&e.SN==priest.WorkObjectSN)
        return std::hypot(e.DR-priest.DR,e.UR-priest.UR)
            >(double(DIS_PRIEST)+1.0)*BLOCKSIDELENGTH;
    return false;
}
// 防守射程及短追击边界。射程外的高优先级目标不能把整条防线拉走。
static double defenseFireRange(const tagArmy& a)
{
    if(a.Sort==AT_STONE_THROWER)return 10;
    if(a.Sort==AT_COMPOSITE_BOWMAN || a.Sort==AT_CHARIOT_ARCHER)return 7;
    if(a.Sort==AT_BOWMAN)return 5;
    if(a.Sort==AT_SLINGER)return 4;
    return 1;
}
// 已转换的投石新炮弹仍会伤友军；目标周围留出爆炸半径及单位碰撞半径。
static bool defenseStoneSafe(const tagInfo& info,const tagArmy& soldier,int target)
{
    if(soldier.Sort!=AT_STONE_THROWER)return true;
    double x=0,y=0;bool found=false;
    for(const auto& e:info.enemy_armies)if(e.SN==target&&e.Blood>0){x=e.DR;y=e.UR;found=true;break;}
    if(!found)for(const auto& b:info.enemy_buildings)if(b.SN==target&&b.Blood>0){
        if(b.Type!=BUILDING_ARROWTOWER)return false; // 不炮击胜利工厂等未知占地建筑
        x=(b.BlockDR+0.5)*BLOCKSIDELENGTH;y=(b.BlockUR+0.5)*BLOCKSIDELENGTH;found=true;break;
    }
    if(!found)return false;
    for(const auto& a:info.armies)if(a.Blood>0&&a.SN!=soldier.SN
        &&std::hypot(a.DR-x,a.UR-y)<1.5*BLOCKSIDELENGTH)return false;
    for(const auto& f:info.farmers)if(f.Blood>0
        &&std::hypot(f.DR-x,f.UR-y)<1.5*BLOCKSIDELENGTH)return false;
    return true;
}
static bool defenseWithinFormation(const tagInfo& info,const tagArmy& soldier,
                                    const tagArmy& enemy,const tagArmy* priest)
{
    const double distance=std::hypot(soldier.DR-enemy.DR,soldier.UR-enemy.UR)/BLOCKSIDELENGTH;
    if(priest&&enemy.WorkObjectSN==priest->SN
        &&std::hypot(enemy.DR-priest->DR,enemy.UR-priest->UR)<=16*BLOCKSIDELENGTH
        &&std::hypot(soldier.DR-priest->DR,soldier.UR-priest->UR)<=12*BLOCKSIDELENGTH)
        return distance<=((defenseRanged(soldier)||soldier.Sort==AT_STONE_THROWER)?defenseFireRange(soldier)+5:10);
    // 第三波收尾：孤立投石不能因塔已倒、祭司后撤而永久留在护卫圈外。
    if(info.GameFrame>=FRAME_WAVE3 && priest && enemy.Sort==AT_STONE_THROWER
        &&std::hypot(enemy.DR-priest->DR,enemy.UR-priest->UR)<=22*BLOCKSIDELENGTH){
        int stones=0,others=0,friends=0;
        for(const auto& e:info.enemy_armies)if(e.Blood>0
            &&std::hypot(e.DR-priest->DR,e.UR-priest->UR)<=25*BLOCKSIDELENGTH){
            if(e.Sort==AT_STONE_THROWER)++stones;else ++others;
        }
        for(const auto& a:info.armies)if(a.Blood>0&&a.Sort!=AT_PRIEST&&a.Sort!=AT_SCOUT
            &&std::hypot(a.DR-priest->DR,a.UR-priest->UR)<=12*BLOCKSIDELENGTH)++friends;
        if(stones>0&&stones<=2&&others==0&&friends>=3)
            return distance<=((defenseRanged(soldier)||soldier.Sort==AT_STONE_THROWER)?defenseFireRange(soldier)+3:22);
    }
    if(defenseRanged(soldier)||soldier.Sort==AT_STONE_THROWER){
        const double range=defenseFireRange(soldier);
        if(distance<=range+0.5)return true;
        if(!priest || distance>range+3
            ||std::hypot(enemy.DR-priest->DR,enemy.UR-priest->UR)>16*BLOCKSIDELENGTH)return false;
        int local=0,friends=0;
        for(const auto& e:info.enemy_armies)if(e.Blood>0
            &&std::hypot(e.DR-priest->DR,e.UR-priest->UR)<=18*BLOCKSIDELENGTH)++local;
        for(const auto& a:info.armies)if(a.Blood>0&&a.Sort!=AT_PRIEST&&a.Sort!=AT_SCOUT
            &&std::hypot(a.DR-priest->DR,a.UR-priest->UR)<=10*BLOCKSIDELENGTH)++friends;
        return local<=5 && friends>=6;
    }
    if(distance<=4)return true;
    // 剩余少量敌军时才允许近战清理投石；仍须在护卫圈内，不能追入敌营。
    if(enemy.Sort!=AT_STONE_THROWER || !priest || distance>8
        || std::hypot(enemy.DR-priest->DR,enemy.UR-priest->UR)>10*BLOCKSIDELENGTH)return false;
    int local=0,friends=0;
    for(const auto& e:info.enemy_armies)if(e.Blood>0
        &&std::hypot(e.DR-priest->DR,e.UR-priest->UR)<=14*BLOCKSIDELENGTH)++local;
    for(const auto& a:info.armies)if(a.Blood>0&&a.Sort!=AT_PRIEST&&a.Sort!=AT_SCOUT
        &&std::hypot(a.DR-priest->DR,a.UR-priest->UR)<=10*BLOCKSIDELENGTH)++friends;
    return local<=3 && friends>=3;
}

static bool defensePursuerNear(const tagArmy& enemy,const tagArmy& priest)
{
    if(enemy.Blood<=0 || enemy.WorkObjectSN!=priest.SN)return false;
    const double margin=(defenseRanged(enemy)||enemy.Sort==AT_STONE_THROWER)?defenseFireRange(enemy)+8:8;
    return std::hypot(enemy.DR-priest.DR,enemy.UR-priest.UR)<=margin*BLOCKSIDELENGTH;
}

struct DefenseStoneEscape { int frame,x,y; };
static std::map<int,DefenseStoneEscape> m_defenseStoneEscapes;
static int m_defenseDodgeTraceFrame=-9999;
static bool defenseDodgeStone(UsrAI* self,const tagInfo& info,const tagArmy& bow,double homeX,double homeY)
{
    if(!defenseRanged(bow))return false;
    const auto last=m_defenseStoneEscapes.find(bow.SN);
    if(last!=m_defenseStoneEscapes.end() && info.GameFrame-last->second.frame<75
        && bow.NowState==HUMAN_STATE_WALKING)return true;
    if(last!=m_defenseStoneEscapes.end() && info.GameFrame-last->second.frame<50)return false;
    const tagArmy* stone=nullptr;const tagArmy* aim=nullptr;
    for(const auto& e:info.enemy_armies)if(e.Blood>0 && e.Sort==AT_STONE_THROWER){
        for(const auto& a:info.armies)if(a.Blood>0 && a.SN==e.WorkObjectSN
            && std::hypot(e.DR-a.DR,e.UR-a.UR)<=11*BLOCKSIDELENGTH
            && std::hypot(bow.DR-a.DR,bow.UR-a.UR)<=1.2*BLOCKSIDELENGTH){
            if(!stone || e.SN<stone->SN){stone=&e;aim=&a;}
        }
    }
    if(!stone)return false;
    const int sx=int(bow.DR/BLOCKSIDELENGTH),sy=int(bow.UR/BLOCKSIDELENGTH);
    if(sx<1||sy<1||sx>=99||sy>=99)return false;
    const double vx=aim->DR-stone->DR,vy=aim->UR-stone->UR;
    const double length=std::hypot(vx,vy);if(length<1)return false;
    auto open=[](int x,int y){return x>=1&&y>=1&&x<99&&y<99
        &&(m_map[x][y]==0||m_map[x][y]==200);};
    bool seen[10000]={false};int queue[10000],head=0,tail=0;
    seen[sx*100+sy]=true;queue[tail++]=sx*100+sy;
    bool found=false;double best=-1e18;int gx=0,gy=0;
    while(head<tail){
        const int at=queue[head++],x=at/100,y=at%100;
        const double px=(x+0.5)*BLOCKSIDELENGTH,py=(y+0.5)*BLOCKSIDELENGTH;
        const double travel=std::hypot(px-bow.DR,py-bow.UR)/BLOCKSIDELENGTH;
        const double side=std::fabs((px-bow.DR)*vy-(py-bow.UR)*vx)/length/BLOCKSIDELENGTH;
        if(open(x,y) && travel>=1.5 && travel<=3.5 && side>=1.5
            && std::hypot(px-aim->DR,py-aim->UR)>=1.6*BLOCKSIDELENGTH
            && std::hypot(px-stone->DR,py-stone->UR)>=std::hypot(bow.DR-stone->DR,bow.UR-stone->UR)-0.5*BLOCKSIDELENGTH
            && std::hypot(px-homeX,py-homeY)<=std::hypot(bow.DR-homeX,bow.UR-homeY)+3*BLOCKSIDELENGTH){
            bool reserved=false;int crowd=0,danger=0;
            for(const auto& move:m_defenseStoneEscapes)if(move.first!=bow.SN
                && info.GameFrame-move.second.frame<75
                && std::hypot(x-move.second.x,y-move.second.y)<1.2)reserved=true;
            for(const auto& a:info.armies)if(a.SN!=bow.SN&&a.Blood>0
                && std::hypot(px-a.DR,py-a.UR)<1.2*BLOCKSIDELENGTH)++crowd;
            for(const auto& e:info.enemy_armies)if(e.Blood>0 && !defenseRanged(e)
                && e.Sort!=AT_STONE_THROWER && std::hypot(px-e.DR,py-e.UR)<3*BLOCKSIDELENGTH)++danger;
            const double score=-danger*100-crowd*20+side*2-travel;
            if(!reserved && score>best){best=score;gx=x;gy=y;found=true;}
        }
        for(int dx=-1;dx<=1;++dx)for(int dy=-1;dy<=1;++dy){
            const int nx=x+dx,ny=y+dy;
            if((!dx&&!dy)||!open(nx,ny)||std::hypot(nx-sx,ny-sy)>4)continue;
            if(dx&&dy&&(!open(x+dx,y)||!open(x,y+dy)))continue;
            const int next=nx*100+ny;if(!seen[next]){seen[next]=true;queue[tail++]=next;}
        }
    }
    if(!found)return false;
    self->HumanMove(bow.SN,(gx+0.5)*BLOCKSIDELENGTH,(gy+0.5)*BLOCKSIDELENGTH);
    m_defenseStoneEscapes[bow.SN]={info.GameFrame,gx,gy};m_issued.insert(bow.SN);
    if(aiTraceEnabled() && info.GameFrame-m_defenseDodgeTraceFrame>=100){
        m_defenseDodgeTraceFrame=info.GameFrame;
        std::cout<<"[DEF_DODGE] f="<<info.GameFrame<<" bow="<<bow.SN<<" stone="<<stone->SN
            <<" aim="<<aim->SN<<" goal="<<gx<<","<<gy<<std::endl;
    }
    return true;
}

// 只在防守危急时使用：首先减少可立即攻击祭司的敌人，其次寻找友军和箭塔掩护。
static bool defenseRetreatPoint(const tagInfo& info,const tagArmy& priest,
                               const Point& anchor,Point& out)
{
    const int sx=int(priest.DR/BLOCKSIDELENGTH),sy=int(priest.UR/BLOCKSIDELENGTH);
    if(sx<1||sy<1||sx>=99||sy>=99)return false;
    bool seen[10000]={false};int queue[200],head=0,tail=0;
    queue[tail++]=sx*100+sy;seen[sx*100+sy]=true;
    auto open=[](int x,int y){return x>=1&&y>=1&&x<99&&y<99
        &&(m_map[x][y]==0||m_map[x][y]==200);};
    bool found=false;double best=-1e18;
    while(head<tail){
        const int at=queue[head++],x=at/100,y=at%100;
        const double travel=std::hypot(x-sx,y-sy);
        if(travel>=2 && open(x,y)){
            double clearance=20,tower=20;int danger=0,cover=0;
            const double px=(x+0.5)*BLOCKSIDELENGTH,py=(y+0.5)*BLOCKSIDELENGTH;
            for(const auto& e:info.enemy_armies)if(e.Blood>0){
                const double distance=std::hypot(px-e.DR,py-e.UR)/BLOCKSIDELENGTH;
                clearance=std::min(clearance,distance);
                if(distance<=(defenseRanged(e)?defenseFireRange(e)+1.0:3.0))++danger;
            }
            for(const auto& a:info.armies)if(a.Blood>0&&a.Sort!=AT_PRIEST&&a.Sort!=AT_SCOUT
                &&std::hypot(px-a.DR,py-a.UR)<=6*BLOCKSIDELENGTH)++cover;
            for(const auto& b:info.buildings)if(b.Type==BUILDING_ARROWTOWER&&b.Percent==100&&b.Blood>0)
                tower=std::min(tower,std::hypot(px-b.BlockDR*BLOCKSIDELENGTH,py-b.BlockUR*BLOCKSIDELENGTH)/BLOCKSIDELENGTH);
            const double score=-danger*50+std::min(clearance,12.0)*4+std::min(cover,4)*3
                +std::max(0.0,7.0-tower)-std::hypot(x-anchor.x,y-anchor.y)*0.2-travel*0.3;
            if(score>best){best=score;out=Point(x,y);found=true;}
        }
        for(int dx=-1;dx<=1;++dx)for(int dy=-1;dy<=1;++dy){
            const int nx=x+dx,ny=y+dy;
            if((!dx&&!dy)||!open(nx,ny)||std::hypot(nx-sx,ny-sy)>6)continue;
            if(dx&&dy&&(!open(x+dx,y)||!open(x,y+dy)))continue;
            const int index=nx*100+ny;if(!seen[index]){seen[index]=true;queue[tail++]=index;}
        }
    }
    return found;
}
static Point m_defenseRetreatGoal(-1,-1);
static int m_defenseRetreatFrame=-9999;

struct DefensePriestCast {int target=-1,start=-1,frame=-1;double x=0,y=0;};
static DefensePriestCast m_defenseCast;
// WorkObjectSN也可能是自动追赶；只保护射程内、连续快照原地施法的转换。
static bool defenseHoldConversion(const tagInfo& info,const tagArmy& priest)
{
    bool inRange=false;
    if(priest.ConvertCooldown<=0)for(const auto& e:info.enemy_armies)
        if(e.Blood>0&&e.SN==priest.WorkObjectSN
            &&std::hypot(e.DR-priest.DR,e.UR-priest.UR)<=(DIS_PRIEST-1)*BLOCKSIDELENGTH){inRange=true;break;}
    if(!inRange){m_defenseCast=DefensePriestCast();return false;}
    if(m_defenseCast.target!=priest.WorkObjectSN||m_defenseCast.frame<0
        ||info.GameFrame-m_defenseCast.frame>25
        ||std::hypot(priest.DR-m_defenseCast.x,priest.UR-m_defenseCast.y)>0.1*BLOCKSIDELENGTH)
        m_defenseCast.start=-1;
    else if(m_defenseCast.start<0){
        m_defenseCast.start=info.GameFrame;
        if(aiTraceEnabled())std::cout<<"[DEF_CAST] f="<<info.GameFrame<<" target="<<priest.WorkObjectSN
            <<" hp="<<priest.Blood<<std::endl;
    }
    m_defenseCast.target=priest.WorkObjectSN;m_defenseCast.frame=info.GameFrame;
    m_defenseCast.x=priest.DR;m_defenseCast.y=priest.UR;
    return m_defenseCast.start>=0&&info.GameFrame-m_defenseCast.start<=165
        &&priest.Blood>=std::max(10,priest.MaxBlood/10);
}
static int m_defenseCancelFrame=-9999;
void UsrAI::defense(const tagInfo& info)
{
    for(auto it=m_towerOrders.begin();it!=m_towerOrders.end();){
        auto ret=info.ins_ret.find(it->second.id);
        if(ret!=info.ins_ret.end() && ret->second!=ACTION_SUCCESS)it=m_towerOrders.erase(it);
        else ++it;
    }
    const tagArmy* priest = nullptr;
    for (const auto& a : info.armies)
        if (a.Sort == AT_PRIEST && a.Blood > 0) { priest = &a; break; }
    int hx, hy; getPriestHome(info, hx, hy);
    double homeX = (hx + 0.5) * BLOCKSIDELENGTH;
    double homeY = (hy + 0.5) * BLOCKSIDELENGTH;
    if (m_enemyDirX == 0 && m_enemyDirY == 0) {
        for (const auto& e : info.enemy_armies) {
            if (e.Blood <= 0 || calDistance(e.DR, e.UR, homeX, homeY) > 24 * BLOCKSIDELENGTH) continue;
            m_enemyDirX = e.DR > homeX ? 1 : -1;
            m_enemyDirY = e.UR > homeY ? 1 : -1;
            break;
        }
    }
    const bool formedDefense=info.GameFrame>=FRAME_WAVE2-3000;
    // 塔倒或祭司正在脱离包围时，护卫随祭司；避免留在旧塔位置任其独自受追击。
    if(formedDefense && priest && priest->Blood<priest->MaxBlood*3/5
        && calDistance(priest->DR,priest->UR,homeX,homeY)>6*BLOCKSIDELENGTH){
        hx=int(priest->DR/BLOCKSIDELENGTH);hy=int(priest->UR/BLOCKSIDELENGTH);
        homeX=(hx+0.5)*BLOCKSIDELENGTH;homeY=(hy+0.5)*BLOCKSIDELENGTH;
    }
    // 根据敌人的实际攻击对象拉仇恨，不能用“已经掉血”推断它正在攻击塔。
    for (const auto& b : info.buildings) {
        if (b.Type != BUILDING_ARROWTOWER || b.Percent < 100 || b.Blood <= 0 || m_issued.count(b.SN)) continue;
        const tagArmy* best = nullptr; double score = -1e18;
        for (const auto& e : info.enemy_armies) {
            if (e.Blood <= 0) continue;
            const double d = calDistance(e.DR, e.UR, b.BlockDR * BLOCKSIDELENGTH, b.BlockUR * BLOCKSIDELENGTH);
            if (d > DIS_ARROWTOWER * BLOCKSIDELENGTH) continue;
            const double priority = (e.WorkObjectSN != b.SN ? 1000 : 0)
                + (priest && e.WorkObjectSN == priest->SN ? 2000 : 0)
                + (defenseRanged(e) ? 100 : 0) - d / BLOCKSIDELENGTH;
            if (priority > score) { score = priority; best = &e; }
        }
        if (!best || b.Project == best->SN) continue;
        const auto pending=m_towerOrders.find(b.SN);
        if(pending!=m_towerOrders.end() && pending->second.target==best->SN)continue;
        const auto last = m_towerSwitch.find(b.SN);
        if (last != m_towerSwitch.end() && info.GameFrame - last->second < 30) continue;
        const int id=HumanAction(b.SN,best->SN);
        m_towerOrders[b.SN]={best->SN,id};
        m_issued.insert(b.SN);m_towerSwitch[b.SN]=info.GameFrame;
    }
    if (m_atkOn) return;
    std::map<int,int> assigned;
    for (const auto& a : info.armies)
        if (a.Sort != AT_PRIEST && a.Blood > 0 && a.WorkObjectSN > 0) ++assigned[a.WorkObjectSN];
    std::vector<const tagArmy*> soldiers;
    for (const auto& a : info.armies)
        if (a.Sort != AT_PRIEST && a.Sort != AT_SCOUT && a.Blood > 0) soldiers.push_back(&a);
    std::sort(soldiers.begin(), soldiers.end(), [](const tagArmy* a, const tagArmy* b){return a->SN < b->SN;});
    for (const tagArmy* a : soldiers) {
        if ((a->SN == m_scoutUnitSN && !m_scoutDone) || m_issued.count(a->SN)) continue;
        if(a->Sort==AT_STONE_THROWER&&a->WorkObjectSN>0&&!defenseStoneSafe(info,*a,a->WorkObjectSN)){
            HumanMove(a->SN,a->DR,a->UR);m_issued.insert(a->SN);m_armySwitch[a->SN]=info.GameFrame;continue;
        }
        if(defenseDodgeStone(this,info,*a,homeX,homeY))continue;
        const auto last = m_armySwitch.find(a->SN);
        if (last != m_armySwitch.end() && info.GameFrame - last->second < 30) continue;
        // 远程被近战贴身时往后拉两格，避免三波近战与投石范围伤害一起吃满。
        const tagArmy* closeMelee = nullptr;
        double closest = 3.0 * BLOCKSIDELENGTH;
        if (defenseRanged(*a)||a->Sort==AT_STONE_THROWER) for (const auto& e : info.enemy_armies) {
            if (e.Blood <= 0 || defenseRanged(e) || e.Sort == AT_STONE_THROWER) continue;
            const double d = calDistance(a->DR, a->UR, e.DR, e.UR);
            if (d < closest) { closest = d; closeMelee = &e; }
        }
        if (closeMelee) {
            double dx = a->DR - closeMelee->DR, dy = a->UR - closeMelee->UR;
            double length = std::hypot(dx,dy);
            if (length < 0.1) { dx = homeX - closeMelee->DR; dy = homeY - closeMelee->UR; length = std::hypot(dx,dy); }
            if (length > 0.1) {
                double gx = a->DR + dx / length * 2 * BLOCKSIDELENGTH;
                double gy = a->UR + dy / length * 2 * BLOCKSIDELENGTH;
                gx = std::max(1.5 * BLOCKSIDELENGTH, std::min(((double)MAP_L - 1.5) * BLOCKSIDELENGTH, gx));
                gy = std::max(1.5 * BLOCKSIDELENGTH, std::min(((double)MAP_U - 1.5) * BLOCKSIDELENGTH, gy));
                adjustReachableTarget(gx,gy);
                HumanMove(a->SN,gx,gy); m_issued.insert(a->SN); m_armySwitch[a->SN] = info.GameFrame;
                continue;
            }
        }
        int target = -1, bestRank = -1, currentRank = -1;
        double bestScore = -1e18;
        for (const auto& e : info.enemy_armies) {
            if (e.Blood <= 0) continue;
            if(!defenseStoneSafe(info,*a,e.SN))continue;
            const double dh = calDistance(e.DR,e.UR,homeX,homeY);
            const double dp = priest ? calDistance(e.DR,e.UR,priest->DR,priest->UR) : 1e18;
            if (dh > 24 * BLOCKSIDELENGTH && dp > 12 * BLOCKSIDELENGTH) continue;
            const double d = calDistance(e.DR,e.UR,a->DR,a->UR);
            if(formedDefense && !defenseWithinFormation(info,*a,e,priest))continue;
            int rank = a->Sort==AT_STONE_THROWER?3:1;
            if (defenseRanged(*a)||a->Sort==AT_STONE_THROWER) {
                if (defenseRanged(e)) rank = 6;
                if (e.Sort == AT_CHARIOT_ARCHER) rank = 7;
                if (e.Sort == AT_STONE_THROWER) rank = 3;
                if (priest && e.WorkObjectSN == priest->SN) rank = 10;
            } else {
                if (defenseRanged(e)) rank = 5;
                if (e.Sort == AT_STONE_THROWER) rank = 8; // 近战必须主动清塔射程外的投石车
                if (priest && e.WorkObjectSN == priest->SN && dp < 6 * BLOCKSIDELENGTH) rank = 10;
            }
            if (rank == 1 && d > 6 * BLOCKSIDELENGTH) continue;
            if (e.SN == a->WorkObjectSN) currentRank = rank;
            const double value = rank * 1000 - assigned[e.SN] * 50 - d / BLOCKSIDELENGTH;
            if (value > bestScore) { bestScore = value; target = e.SN; bestRank = rank; }
        }
        if (target >= 0) {
            // 只保留同等/更高优先级目标；正在打别的存活单位不是拒绝救塔的理由。
            if (a->WorkObjectSN == target || (a->NowState == HUMAN_STATE_ATTACKING && currentRank >= bestRank)) continue;
            HumanAction(a->SN,target); m_issued.insert(a->SN); m_armySwitch[a->SN] = info.GameFrame;
            if (a->WorkObjectSN > 0) --assigned[a->WorkObjectSN];
            ++assigned[target];
        } else if (a->NowState == HUMAN_STATE_IDLE
            || (formedDefense && a->WorkObjectSN>0)) {
            // 使用稳定的不同格子布防，避免整队挤在一个点。
            int slot = 0; for (const auto* b : soldiers) if (b->SN < a->SN) ++slot;
            int bx = hx + slot % 5 - 2, by = hy + slot / 5 - 2;
            if (isStaticBlock(bx,by)) { bx = hx; by = hy; }
            double gx = (bx + 0.5) * BLOCKSIDELENGTH, gy = (by + 0.5) * BLOCKSIDELENGTH;
            adjustReachableTarget(gx,gy);
            if (calDistance(a->DR,a->UR,gx,gy) > 2 * BLOCKSIDELENGTH) {
                HumanMove(a->SN,gx,gy); m_issued.insert(a->SN); m_armySwitch[a->SN] = info.GameFrame;
            } else if(formedDefense && a->WorkObjectSN>0){
                HumanMove(a->SN,a->DR,a->UR);m_issued.insert(a->SN);m_armySwitch[a->SN]=info.GameFrame;
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
static bool atkRetreatPoint(const tagInfo& info, const tagArmy& priest, const Point& anchor, Point& out);
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

    if(defenseHoldConversion(info,*priest)){
        m_priestLastBlood=priest->Blood;m_issued.insert(priestSN);
        return; // 保留2～6秒施法，不因六成血量/多个锁定者提前重置转换计时。
    }

    if(info.GameFrame>=FRAME_WAVE2-3000 && defenseConversionTooFar(info,*priest)){
        // 目标跑出施法范围时取消自动追击；同帧和快照延迟期间不再接新转换令。
        if(info.GameFrame-m_defenseCancelFrame>=25){
            int hx,hy;getPriestHome(info,hx,hy);
            if(!movePriest(priestSN,priest->DR,priest->UR,hx*BLOCKSIDELENGTH,
                hy*BLOCKSIDELENGTH,info.GameFrame))HumanMove(priestSN,priest->DR,priest->UR);
            m_defenseCancelFrame=info.GameFrame;
            if(aiTraceEnabled())std::cout<<"[DEF_CANCEL] f="<<info.GameFrame
                <<" target="<<priest->WorkObjectSN<<" reason=outside_conversion_range"<<std::endl;
        }
        m_issued.insert(priestSN);m_convertTarget=-1;m_convertStartFrame=-1;
        return;
    }

    // 防守低血撤退也按实际追兵、友军和可达地形选点，避免穿过近战直奔远处塔。
    int selfLocks=0,rangedLocks=0;bool danger=false,activeConvert=false;
    const bool retreatInProgress=m_defenseRetreatGoal.x>=0
        &&info.GameFrame-m_defenseRetreatFrame<100
        &&calDistance(priest->DR,priest->UR,(m_defenseRetreatGoal.x+0.5)*BLOCKSIDELENGTH,
            (m_defenseRetreatGoal.y+0.5)*BLOCKSIDELENGTH)>1.5*BLOCKSIDELENGTH;
    for(const auto& e:info.enemy_armies) if(e.Blood>0) {
        if(defensePursuerNear(e,*priest)){
            ++selfLocks;danger=true;if(defenseRanged(e))++rangedLocks;
        }
        if(calDistance(e.DR,e.UR,priest->DR,priest->UR)<6*BLOCKSIDELENGTH) danger=true;
        if(e.SN==priest->WorkObjectSN && priest->ConvertCooldown<=0) activeConvert=true;
    }
    if(info.GameFrame>=FRAME_WAVE2-3000
        &&((priest->Blood<priest->MaxBlood*3/5 && (danger||retreatInProgress))
            ||(priest->ConvertCooldown>0 && rangedLocks>0))
        && (!activeConvert || selfLocks>=2 || priest->Blood<priest->MaxBlood/4)) {
        int hx,hy;getPriestHome(info,hx,hy);Point retreat;
        const bool keep=retreatInProgress;
        if(keep || defenseRetreatPoint(info,*priest,Point(hx,hy),retreat)) {
            if(keep)retreat=m_defenseRetreatGoal;
            else{m_defenseRetreatGoal=retreat;m_defenseRetreatFrame=info.GameFrame;}
            if(movePriest(priestSN,priest->DR,priest->UR,(retreat.x+0.5)*BLOCKSIDELENGTH,
                (retreat.y+0.5)*BLOCKSIDELENGTH,info.GameFrame))m_convertStartFrame=-1;
            m_issued.insert(priestSN);
            return;
        }
    }

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
            bool threatened = false;
            for (const auto& e : info.enemy_armies)
                if (e.Blood > 0 && (e.WorkObjectSN == priestSN
                    || calDistance(e.DR, e.UR, priest->DR, priest->UR) < 7 * BLOCKSIDELENGTH))
                    threatened = true;
            if (lhx >= 0 && (priest->NowState == HUMAN_STATE_IDLE
                || (threatened && priest->Blood < priest->MaxBlood * 3 / 5))) {
                movePriest(priestSN, priest->DR, priest->UR,
                           (double)lhx * BLOCKSIDELENGTH, (double)lhy * BLOCKSIDELENGTH,
                           info.GameFrame);
                return;
            }
            // 忙碌状态继续检测危险，不能因超出拴绳而跳过后面的撤退。
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
                                );
    int lockedRangedSN = -1;      // 锁定祭司的"远程兵"（含战车弓）
    int lockedAnySN = -1;         // 锁定祭司的任意敌人
    if (wave2Defense) {
        for (const tagArmy& e : info.enemy_armies) {                 // 第一优先：战车弓兵
            if (e.Blood <= 0 || calDistance(e.DR,e.UR,priest->DR,priest->UR)>11*BLOCKSIDELENGTH) continue;
            if (e.WorkObjectSN != priestSN) continue;
            if (e.Sort == AT_CHARIOT_ARCHER) { lockedRangedSN = e.SN; break; }
        }
        if (lockedRangedSN < 0) {
            for (const tagArmy& e : info.enemy_armies) {             // 其次：其它远程兵
                if (e.Blood <= 0 || calDistance(e.DR,e.UR,priest->DR,priest->UR)>11*BLOCKSIDELENGTH) continue;
                if (e.WorkObjectSN != priestSN) continue;
                bool ranged = (e.Sort == AT_BOWMAN || e.Sort == AT_COMPOSITE_BOWMAN
                               || e.Sort == AT_SLINGER);
                if (ranged) { lockedRangedSN = e.SN; break; }
                if (lockedAnySN < 0) lockedAnySN = e.SN;             // 顺带记住近战锁定者
            }
        }
    }

    int lockedRangedCount = 0;
    double lockedX = 0, lockedY = 0;
    for (const auto& e : info.enemy_armies) if (e.Blood > 0 && e.WorkObjectSN == priestSN
        && calDistance(e.DR,e.UR,priest->DR,priest->UR)<=11*BLOCKSIDELENGTH
        && (e.Sort == AT_CHARIOT_ARCHER || e.Sort == AT_BOWMAN || e.Sort == AT_COMPOSITE_BOWMAN || e.Sort == AT_SLINGER)) {
        ++lockedRangedCount; lockedX += e.DR; lockedY += e.UR;
    }
    if (wave2Defense && lockedRangedCount >= 2 && (!inConversion || priest->Blood < priest->MaxBlood * 3 / 5)) {
        int hx, hy; getPriestHome(info,hx,hy);
        double gx=hx*BLOCKSIDELENGTH, gy=hy*BLOCKSIDELENGTH;
        double dx=gx-lockedX/lockedRangedCount, dy=gy-lockedY/lockedRangedCount;
        const double length=std::hypot(dx,dy);
        if (length>0.1) { gx+=dx/length*4*BLOCKSIDELENGTH; gy+=dy/length*4*BLOCKSIDELENGTH; }
        movePriest(priestSN,priest->DR,priest->UR,gx,gy,info.GameFrame);
        m_convertStartFrame=-1; return;
    }
    if (wave2Defense && !inConversion && priest->ConvertCooldown > 0 && lockedRangedSN >= 0) {
        // 转化后的冷却期无法再自救；被弓兵锁定即撤到塔背面，不等血量跌破六成。
        for (const auto& e : info.enemy_armies) if (e.SN == lockedRangedSN) {
            int hx, hy; getPriestHome(info, hx, hy);
            double gx, gy;
            priestRetreatLatched(1, e.DR, e.UR, hx, hy, priest->DR, priest->UR, info.GameFrame, gx, gy);
            movePriest(priestSN, priest->DR, priest->UR, gx, gy, info.GameFrame);
            return;
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
            if(wave2Defense)attackerSN=defenseSelfConvertTarget(info,*priest,attackerSN);
            if (!wave2Defense && attackerSN < 0) {
                for (const tagArmy& e : info.enemy_armies) {
                    if (e.Blood <= 0) continue;
                    if (e.WorkObjectSN == priestSN) { attackerSN = e.SN; break; }   // 正在打我
                }
            }
            if (!wave2Defense && attackerSN < 0) {
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
        const bool wave2LureWindow = (info.GameFrame > FRAME_WAVE2 - 3000 && info.GameFrame < FRAME_WAVE3);
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
    if (!inConversion && threat != nullptr && nearest < threatDist) {
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
static int m_scoutContactX = -1, m_scoutContactY = -1; // 首次接触敌军的位置
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

// 路点：第一趟优先自家基地的对角区域，来袭部队方向不代表敌方主营位置。
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
    // 对角区域没找到时继续原有四角轮换，不把主营固定在单个坐标。
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

    if (m_scoutUnitSN >= 0) {
        const tagArmy* scout = nullptr;
        for (const auto& a : info.armies)
            if (a.SN == m_scoutUnitSN && a.Blood > 0) { scout = &a; break; }
        if (scout) {
            m_scoutLastX = scout->BlockDR; m_scoutLastY = scout->BlockUR;
            bool contact = false;
            for (const auto& e : info.enemy_armies)
                if (e.Blood > 0 && (e.WorkObjectSN == scout->SN
                    || self->calDistance(e.DR, e.UR, scout->DR, scout->UR)
                        <= 12 * BLOCKSIDELENGTH)) { contact = true; break; }
            if (contact && m_scoutContactX < 0) {
                m_scoutContactX = scout->BlockDR; m_scoutContactY = scout->BlockUR;
            }
        } else {
            if (m_scoutDeathX < 0 && m_scoutLastX >= 0) {
                m_scoutDeathX = m_scoutLastX; m_scoutDeathY = m_scoutLastY;
            }
            m_scoutUnitSN = -1; ++m_scoutLost; ++m_scoutLeg;
        }
    }

    // ---------- 0) 【诊断】从 15000 帧起每 600 帧（24 秒）打一行到 stdout：
    //   本地 Qt Creator 的"应用程序输出"面板、以及 OJ 的标准输出日志都能看到，
    //   直接说明"派没派 / 为什么没派"。日志太多时把这几行删掉即可（不影响逻辑）。
    if (aiTraceEnabled() && f >= 15000 && f - m_scoutDbgFrame >= 600) {
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
#if USRAI_DEBUG_LINE
        self->DebugText(QString("[SCOUT]f%1 done%2 unit%3 army%4/%5 idle%6 bld%7 siege%8 expl%9")
                        .arg(f).arg(m_scoutDone).arg(m_scoutUnitSN)
                        .arg(scoutArmyCount(info)).arg(SCOUT_MIN_ARMY).arg(nIdle)
                        .arg(m_scoutBldSeen).arg(m_siegeSN).arg(m_scoutExplPct));
#endif
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
    if (m_scoutBldSeen >= 2) {                // 条件B：≥2 个敌方建筑 → 已定位主营 → 收工
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
        if(aiTraceEnabled()) std::cout << "[SCOUT_END] f=" << f << " why=" << m_scoutDoneWhy
                  << " (1=看到厂 2=敌建筑>=2 3=帧 4=死3个 5=探明率)"
                  << " siegeSN=" << m_siegeSN << " bldSeen=" << m_scoutBldSeen
                  << " lost=" << m_scoutLost << " expl=" << m_scoutExplPct << std::endl;
#if USRAI_DEBUG_LINE
        self->DebugText(QString("[SCOUT_END]f%1 why%2 siege%3 bld%4 lost%5 expl%6")
                        .arg(f).arg(m_scoutDoneWhy).arg(m_siegeSN)
                        .arg(m_scoutBldSeen).arg(m_scoutLost).arg(m_scoutExplPct));
#endif
    }
    // 任务已结束 → 本模块不再下令；该兵不再进 m_issued → 自动回归 defense 管理
    if (m_scoutDone || m_atkOn) { m_scoutUnitSN = -1; return; }

    // ---------- 3) 门没开 → 完全不动（前期一行不执行）----------
    if (f < SCOUT_START_FRAME) return;
    // 【用户要求·等第二波打完再探】场上只要有**可见敌人**（敌方单位或敌方农民）就不出发 ✓
    //   否则探路兵跑出去会把第二/三波的进攻部队提前点亮、引来 ✗
    //   ——"第二波进攻结束"就以"看不到敌人了"为准 ✓（简单、不用额外计时器 ✓）
    if (!info.enemy_armies.empty() || !info.enemy_farmers.empty()) return;
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
            if(aiTraceEnabled()) std::cout << "[SCOUT_GO] f=" << f << " scoutSN=" << sn
                      << " wp0=(" << m_scoutWPX << "," << m_scoutWPY << ")"
                      << " army=" << scoutArmyCount(info) << std::endl;
#if USRAI_DEBUG_LINE
            self->DebugText(QString("[SCOUT_GO]f%1 sn%2 wp(%3,%4) army%5")
                            .arg(f).arg(sn).arg(m_scoutWPX).arg(m_scoutWPY)
                            .arg(scoutArmyCount(info)));
#endif
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

// 反攻按攻略执行：第三波后集结，分批诱敌，军队掩护祭司转化工厂。
// 所有状态仍放在文件作用域，不改变 UsrAI 的对象布局。
enum AtkStage { ATK_RALLY, ATK_PROBE, ATK_FIGHT, ATK_CONVERT };
static const int ATK_FACTORY_ESCORT_MIN = 6; // 清场后至少六名实际护卫再靠近工厂。
static const int ATK_MAIN_FRAME = 24000;       // 16 分钟后考虑反攻
static const int ATK_MAIN_ARMY = COUNTER_COMPOSITE_TARGET+COUNTER_FRONT_TARGET;
static const int ATK_LAST_FRAME = 27000;
static const int ATK_LAST_ARMY = ATK_MAIN_ARMY; // 后期也不自动降低主力要求。
static const int ATK_ABORT_ARMY = 6;
static const int ATK_QUIET_FRAMES = 250;       // 接触区域平静 10 秒才推进/转化
static const int ATK_MAX_ORDER = 10;
static int m_atkTargetX = -1, m_atkTargetY = -1;
static int m_atkRallyX = -1, m_atkRallyY = -1;
static int m_atkFrontX = -1, m_atkFrontY = -1;
static int m_atkQuietSince = -1, m_atkHomeQuietSince = -1;
static int m_atkRetryAfter = 0, m_atkLastFrame = -1;
static int m_atkSlotX = -1, m_atkSlotY = -1;
static std::map<int, Point> m_atkSlots;
static std::set<int> m_atkCoreSquad;
static std::vector<Point> m_atkTrail;
static int m_assaultSN = -1, m_assaultStage = 0, m_assaultFrame = -1;
static int m_assaultBlood = 0;
static bool m_assaultPushed = false;
static double m_assaultPX = 0, m_assaultPY = 0;
struct AtkMoveState {
    int orderFrame = -9999, progressFrame = -1;
    double gx = 0, gy = 0, px = 0, py = 0;
    double destinationX=0,destinationY=0;
};
static std::map<int, AtkMoveState> m_atkMoves;
static std::map<int, int> m_atkActions;
static std::map<int, int> m_atkDodgeFrames;
static std::map<int,int> m_atkConvertAvoid;
static int m_atkDbgFrame = -9999;
static int m_atkAdvanceFrame = -9999;
static int m_atkPullbackUntil = -9999,m_atkPullbackFrame = -9999;
static bool m_atkHolding=false,m_atkFinishWithdraw=false;
static int m_atkRouteTraceFrame=-9999;
static int m_atkRetreatTraceFrame=-9999;
static Point m_atkRetreatGoal;
static int m_atkRetreatGoalFrame=-9999;

// 所有军队阶段共用发令公平性；动作与移动都按最后一次实际下令计时。
static int atkLastOrderFrame(int sn)
{
    int frame=-9999;auto move=m_atkMoves.find(sn);auto action=m_atkActions.find(sn);
    if(move!=m_atkMoves.end())frame=move->second.orderFrame;
    if(action!=m_atkActions.end())frame=std::max(frame,action->second);
    return frame;
}
static double atkBD(int b) { return (b + 0.5) * BLOCKSIDELENGTH; }
static double atkDist(double x, double y, double u, double v)
{ return std::hypot(x - u, y - v); }
static bool atkFighter(const tagArmy& a)
{ return a.Blood > 0 && a.Sort != AT_PRIEST && a.Sort != AT_SCOUT; }
static bool atkRanged(const tagArmy& a)
{
    return a.Sort == AT_BOWMAN || a.Sort == AT_COMPOSITE_BOWMAN
        || a.Sort == AT_CHARIOT_ARCHER || a.Sort == AT_SLINGER
        || a.Sort == AT_STONE_THROWER;
}
static int atkArmyCount(const tagInfo& info)
{
    int n = 0;
    for (const auto& a : info.armies) if (atkFighter(a)) ++n;
    return n;
}
static bool atkOpen(int x, int y, bool known = false)
{
    if (x < 0 || y < 0 || x >= std::min(100, (int)MAP_L)
        || y >= std::min(100, (int)MAP_U)) return false;
    const int v = m_map[x][y];
    return v == 0 || v == 200 || (!known && v == -2);
}
// 阵形只能使用前线所在岸的可达空位；局部洪泛不会把隔河近点当同一阵地。
static void atkFormationArea(int x,int y,bool* seen)
{
    std::fill(seen,seen+10000,false);
    if(x<0||y<0||x>=std::min(100,int(MAP_L))||y>=std::min(100,int(MAP_U)))return;
    int queue[10000],head=0,tail=0;seen[x*100+y]=true;queue[tail++]=x*100+y;
    while(head<tail){
        const int at=queue[head++],cx=at/100,cy=at%100;
        for(int dx=-1;dx<=1;++dx)for(int dy=-1;dy<=1;++dy){
            const int nx=cx+dx,ny=cy+dy;
            if((!dx&&!dy)||!atkOpen(nx,ny,true)||std::max(std::abs(nx-x),std::abs(ny-y))>10)continue;
            if(dx&&dy&&(!atkOpen(cx+dx,cy,true)||!atkOpen(cx,cy+dy,true)))continue;
            if(!seen[nx*100+ny]){seen[nx*100+ny]=true;queue[tail++]=nx*100+ny;}
        }
    }
}
static bool atkStraightOpen(int x,int y,int tx,int ty,bool avoidUnits=false)
{
    auto open=[&](int px,int py){return atkOpen(px,py,true)&&(!avoidUnits||m_map[px][py]!=200);};
    while(x!=tx||y!=ty){
        const int dx=tx==x?0:(tx>x?1:-1),dy=ty==y?0:(ty>y?1:-1);
        if(!open(x+dx,y+dy))return false;
        if(dx&&dy&&(!open(x+dx,y)||!open(x,y+dy)))return false;
        x+=dx;y+=dy;
    }
    return true;
}
// 护卫可短步进入射程，但每个路点仍在祭司九格内，射击位置在六格内。
// 只搜索已知可达空地；被水域/树林分隔时不能直接指定隔岸射击点。
static bool atkTowerPosition(const tagArmy& soldier, const tagArmy& priest,
                             const tagBuilding& tower, double range,
                             const std::set<int>& reserved, Point& out, Point& step)
{
    const int sx=(int)(soldier.DR/BLOCKSIDELENGTH),sy=(int)(soldier.UR/BLOCKSIDELENGTH);
    if(sx<0||sy<0||sx>=std::min(100,int(MAP_L))||sy>=std::min(100,int(MAP_U))
        ||atkDist(soldier.DR,soldier.UR,priest.DR,priest.UR)>9*BLOCKSIDELENGTH
        ||atkDist(priest.DR,priest.UR,atkBD(tower.BlockDR),atkBD(tower.BlockUR))
            >(range+6)*BLOCKSIDELENGTH)return false;
    int parent[10000];std::fill(parent,parent+10000,-1);
    int queue[10000],head=0,tail=0;
    const int start=sx*100+sy;parent[start]=start;queue[tail++]=start;
    while(head<tail){
        const int at=queue[head++],x=at/100,y=at%100;
        if(atkOpen(x,y,true)&&!reserved.count(at)
            &&atkDist(atkBD(x),atkBD(y),priest.DR,priest.UR)<=6*BLOCKSIDELENGTH
            &&atkDist(atkBD(x),atkBD(y),atkBD(tower.BlockDR),atkBD(tower.BlockUR))
                <=range*BLOCKSIDELENGTH){
            out=Point(x,y);step=out;
            std::vector<int> path;
            for(int node=at;node!=start;node=parent[node])path.push_back(node);
            if(!path.empty()){
                const int selected=path[path.size()-std::min((int)path.size(),3)];
                step=Point(selected/100,selected%100);
            }
            return true;
        }
        for(int dx=-1;dx<=1;++dx)for(int dy=-1;dy<=1;++dy){
            const int nx=x+dx,ny=y+dy;
            if((!dx&&!dy)||!atkOpen(nx,ny,true)
                ||atkDist(atkBD(nx),atkBD(ny),priest.DR,priest.UR)>9*BLOCKSIDELENGTH)continue;
            if(dx&&dy&&(!atkOpen(x+dx,y,true)||!atkOpen(x,y+dy,true)))continue;
            const int index=nx*100+ny;
            if(parent[index]<0){parent[index]=at;queue[tail++]=index;}
        }
    }
    return false;
}
static bool atkCanLeave(const tagArmy& a)
{
    const int x=(int)(a.DR/BLOCKSIDELENGTH), y=(int)(a.UR/BLOCKSIDELENGTH);
    for(int dx=-1;dx<=1;++dx) for(int dy=-1;dy<=1;++dy) {
        if((!dx&&!dy) || !atkOpen(x+dx,y+dy)) continue;
        if(dx&&dy&&(!atkOpen(x+dx,y)||!atkOpen(x,y+dy))) continue;
        return true;
    }
    return false;
}
static int atkRetreatDanger(const tagInfo& info, const Point& point)
{
    int danger=0;
    for(const auto& b:info.enemy_buildings)if(b.Type==BUILDING_ARROWTOWER&&b.Blood>0
        &&atkDist(atkBD(point.x),atkBD(point.y),atkBD(b.BlockDR),atkBD(b.BlockUR))
            <=(DIS_ARROWTOWER+2)*BLOCKSIDELENGTH)++danger;
    for(const auto& e:info.enemy_armies)if(e.Blood>0
        &&atkDist(atkBD(point.x),atkBD(point.y),e.DR,e.UR)
            <=(atkRanged(e)?defenseFireRange(e)+1:3)*BLOCKSIDELENGTH)++danger;
    return danger;
}
static bool atkRetreatPoint(const tagInfo& info, const tagArmy& priest, const Point& anchor, Point& out)
{
    const int px=(int)(priest.DR/BLOCKSIDELENGTH),py=(int)(priest.UR/BLOCKSIDELENGTH);
    if(px<0||py<0||px>=std::min(100,int(MAP_L))||py>=std::min(100,int(MAP_U))) return false;
    // 安全终点短时保持，不能因祭司已朝它走了一步便反复换向。
    // 新危险、挡路、到达或超时都立即重选，不锁定有威胁的旧目标。
    if(info.GameFrame-m_atkRetreatGoalFrame<100
        &&atkDist(priest.DR,priest.UR,atkBD(m_atkRetreatGoal.x),atkBD(m_atkRetreatGoal.y))>BLOCKSIDELENGTH
        &&atkOpen(m_atkRetreatGoal.x,m_atkRetreatGoal.y,true)
        &&atkStraightOpen(px,py,m_atkRetreatGoal.x,m_atkRetreatGoal.y)
        &&atkRetreatDanger(info,m_atkRetreatGoal)==0){out=m_atkRetreatGoal;return true;}
    bool seen[10000]={};int queue[200],head=0,tail=0;
    const int start=px*100+py;seen[start]=true;queue[tail++]=start;
    bool found=false;double best=-1e18;
    while(head<tail) {
        const int at=queue[head++],x=at/100,y=at%100;
        if(std::hypot(x-px,y-py)>=3) {
            double clearance=1e18,towerDistance=1e18;int cover=0;
            for(const auto& e:info.enemy_armies)if(e.Blood>0)
                clearance=std::min(clearance,atkDist(atkBD(x),atkBD(y),e.DR,e.UR)/BLOCKSIDELENGTH);
            for(const auto& b:info.enemy_buildings)if(b.Type==BUILDING_ARROWTOWER&&b.Blood>0)
                clearance=std::min(clearance,atkDist(atkBD(x),atkBD(y),atkBD(b.BlockDR),atkBD(b.BlockUR))/BLOCKSIDELENGTH);
            for(const auto& a:info.armies)if(atkFighter(a)
                &&atkDist(atkBD(x),atkBD(y),a.DR,a.UR)<=6*BLOCKSIDELENGTH)++cover;
            for(const auto& b:info.buildings)if(b.Type==BUILDING_ARROWTOWER&&b.Blood>0&&b.Percent==100)
                towerDistance=std::min(towerDistance,atkDist(atkBD(x),atkBD(y),atkBD(b.BlockDR),atkBD(b.BlockUR))/BLOCKSIDELENGTH);
            const int danger=atkRetreatDanger(info,Point(x,y));
            const double score=-danger*100+std::min(clearance,12.0)*3+std::min(cover,6)*6
                +std::max(0.0,7.0-towerDistance)*2-std::hypot(x-anchor.x,y-anchor.y)*0.7;
            if(score>best){best=score;out=Point(x,y);found=true;}
        }
        for(int dx=-1;dx<=1;++dx)for(int dy=-1;dy<=1;++dy) {
            const int nx=x+dx,ny=y+dy;
            if((!dx&&!dy)||!atkOpen(nx,ny)||std::hypot(nx-px,ny-py)>6)continue;
            if(dx&&dy&&(!atkOpen(x+dx,y)||!atkOpen(x,y+dy)))continue;
            const int index=nx*100+ny;
            if(!seen[index]){seen[index]=true;queue[tail++]=index;}
        }
    }
    if(found){m_atkRetreatGoal=out;m_atkRetreatGoalFrame=info.GameFrame;}
    return found;
}
static bool atkFindOpen(int x, int y, int radius, Point& out, bool known = false)
{
    for (int r = 0; r <= radius; ++r)
        for (int dx = -r; dx <= r; ++dx)
            for (int dy = -r; dy <= r; ++dy) {
                if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
                if (atkOpen(x + dx, y + dy, known)) {
                    out = Point(x + dx, y + dy); return true;
                }
            }
    return false;
}
static void atkDirection(int x, int y, double& dx, double& dy)
{
    dx = m_atkTargetX - x; dy = m_atkTargetY - y;
    const double len = std::hypot(dx, dy);
    if (len > 0.01) { dx /= len; dy /= len; }
    else { dx = 0; dy = 0; }
}
static Point atkRear(int x, int y)
{
    // 后排沿已经走过的路线布置，不能隔着河或树林选一个直线距离很近的点。
    if (m_atkTrail.size() >= 3) {
        const Point p = m_atkTrail[m_atkTrail.size() - 3];
        if (atkOpen(p.x, p.y)) return p;
    }
    double dx, dy; atkDirection(x, y, dx, dy);
    Point p(x, y), open(x, y);
    p.x -= (int)std::lround(dx * 4); p.y -= (int)std::lround(dy * 4);
    if (atkFindOpen(p.x, p.y, 5, open)) return open;
    return p;
}
static bool atkTowerCoverAt(const tagInfo& info, const Point& position)
{
    for(const auto& b:info.enemy_buildings)if(b.Type==BUILDING_ARROWTOWER&&b.Blood>0
        &&atkDist(atkBD(position.x),atkBD(position.y),atkBD(b.BlockDR),atkBD(b.BlockUR))
            <=(DIS_ARROWTOWER+1)*BLOCKSIDELENGTH){
        bool covered=false;
        for(const auto& a:info.armies)if(atkFighter(a)&&a.SN==b.Project&&a.Blood>=25
            &&atkDist(a.DR,a.UR,atkBD(b.BlockDR),atkBD(b.BlockUR))
                <=(DIS_ARROWTOWER+1)*BLOCKSIDELENGTH)covered=true;
        if(!covered)return false;
    }
    return true;
}
// 以下距离是开始接近工厂的准备门槛；建筑转换仍需走到建筑旁边。
// 此处只判断当前站位受到的塔火力，不能当作接近全程的安全证明。
// 工厂另一侧、打不到祭司的空闲塔不应阻止安全转换。
static bool atkConversionCover(const tagInfo& info,const tagArmy& priest)
{
    if(m_siegeSN<0 || m_siegeX<0 || priest.Blood<priest.MaxBlood/3
        || atkDist(priest.DR,priest.UR,atkBD(m_siegeX),atkBD(m_siegeY))
            >(double(DIS_PRIEST)-1.0)*BLOCKSIDELENGTH)return false;
    for(const auto& b:info.enemy_buildings)if(b.Type==BUILDING_ARROWTOWER&&b.Blood>0
        &&atkDist(priest.DR,priest.UR,atkBD(b.BlockDR),atkBD(b.BlockUR))
            <=(DIS_ARROWTOWER+1)*BLOCKSIDELENGTH){
        bool covered=false;
        for(const auto& a:info.armies)if(atkFighter(a)&&a.SN==b.Project&&a.Blood>=25
            &&atkDist(a.DR,a.UR,atkBD(b.BlockDR),atkBD(b.BlockUR))<=(DIS_ARROWTOWER+1)*BLOCKSIDELENGTH)covered=true;
        if(!covered)return false;
    }
    return true;
}
static bool atkFactoryOpportunity(const tagInfo& info,const tagArmy& priest)
{
    if(m_siegeSN<0 || m_siegeX<0 || priest.ConvertCooldown>0
        ||priest.Blood<std::max(10,priest.MaxBlood/10)
        ||atkDist(priest.DR,priest.UR,atkBD(m_siegeX),atkBD(m_siegeY))>11*BLOCKSIDELENGTH)return false;
    for(const auto& e:info.enemy_armies)if(e.Blood>0)return false;
    int guards=0;bool factoryVisible=false;
    for(const auto& a:info.armies)if(atkFighter(a)){
        if(atkDist(a.DR,a.UR,priest.DR,priest.UR)<=9*BLOCKSIDELENGTH)++guards;
        if(atkDist(a.DR,a.UR,atkBD(m_siegeX),atkBD(m_siegeY))<=6*BLOCKSIDELENGTH)factoryVisible=true;
    }
    for(const auto& b:info.enemy_buildings)if(b.Type==BUILDING_ARROWTOWER&&b.Blood>0
        &&atkDist(priest.DR,priest.UR,atkBD(b.BlockDR),atkBD(b.BlockUR))<=(DIS_ARROWTOWER+1)*BLOCKSIDELENGTH)
        if(priest.Blood<priest.MaxBlood/3 || !atkTowerCoverAt(info,Point(int(priest.DR/BLOCKSIDELENGTH),
            int(priest.UR/BLOCKSIDELENGTH))))return false;
    return guards>=ATK_FACTORY_ESCORT_MIN && factoryVisible;
}

static void atkReset(int frame)
{
    m_atkOn = 0; m_atkPhase = ATK_RALLY;
    m_assaultSN = -1; m_assaultStage = 0;
    m_atkSlots.clear(); m_atkCoreSquad.clear(); m_atkTrail.clear(); m_atkMoves.clear(); m_atkActions.clear(); m_atkDodgeFrames.clear();
    m_atkConvertAvoid.clear();
    m_atkSlotX = m_atkSlotY = -1; m_atkQuietSince = -1;
    m_atkAdvanceFrame = -9999;
    m_atkPullbackUntil=m_atkPullbackFrame=-9999;
    m_atkHolding=false;m_atkFinishWithdraw=false;m_armyReachability.frame=-9999;
    
    m_atkRetreatTraceFrame=-9999;
    m_atkRetreatGoalFrame=-9999;
    m_atkRetryAfter = frame + 750;             // 撤回后至少恢复 30 秒再考虑出征
}
static bool atkRouteStep(int x, int y, int tx, int ty, int step, int stopRadius, Point& out,bool known=false,bool avoidUnits=false);
static bool atkMove(UsrAI* self, const tagInfo& info, const tagArmy& a,
                    double gx, double gy, int& orders, bool urgent = false, double arrival = 1.0)
{
    if (orders >= ATK_MAX_ORDER || m_issued.count(a.SN)) return false;
    if (!urgent && a.NowState != HUMAN_STATE_IDLE
        && a.NowState != HUMAN_STATE_WALKING) return false;
    if (atkDist(a.DR, a.UR, gx, gy) <= arrival * BLOCKSIDELENGTH) return false;
    AtkMoveState& st = m_atkMoves[a.SN];
    const double destinationX=gx,destinationY=gy;
    // 路点只供执行；最终槽位没变时，正在绕路者不能反复占用整队发令名额。
    const bool changed = atkDist(st.destinationX,st.destinationY,gx,gy)>1.0*BLOCKSIDELENGTH;
    if (st.progressFrame < 0 || atkDist(st.px, st.py, a.DR, a.UR) > BLOCKSIDELENGTH) {
        st.progressFrame = info.GameFrame; st.px = a.DR; st.py = a.UR;
    }
    const bool stuck = info.GameFrame - st.progressFrame >= 250;
    if (info.GameFrame - st.orderFrame < (urgent ? 15 : 50)) return false;
    if (!urgent && !changed && a.NowState == HUMAN_STATE_WALKING && !stuck) return false;
    if (stuck && !urgent) {
        // 改走邻近空位，不能靠超时跳过集结，更不能把祭司留下。
        Point p;
        const Point anchor = a.Sort==AT_PRIEST ? atkRear(m_atkFrontX,m_atkFrontY) : Point(m_atkFrontX,m_atkFrontY);
        const int bx=anchor.x, by=anchor.y;
        bool found = false;
        double bestDistance=1e18;
        for (int r = 1; r <= (a.Sort==AT_PRIEST ? 2 : 6); ++r)
            for (int dx = -r; dx <= r; ++dx)
                for (int dy = -r; dy <= r; ++dy) {
                    if (std::max(std::abs(dx), std::abs(dy)) != r
                        || !atkOpen(bx + dx, by + dy)) continue;
                    bool taken = false;
                    for (const auto& slot : m_atkSlots)
                        if (slot.first != a.SN && slot.second.x == bx + dx
                            && slot.second.y == by + dy) { taken = true; break; }
                    const double d=atkDist(a.DR,a.UR,atkBD(bx+dx),atkBD(by+dy));
                    if (!taken && d<bestDistance) { p=Point(bx+dx,by+dy); bestDistance=d; found=true; }
                }
        if (found) {
            gx = atkBD(p.x); gy = atkBD(p.y);
            m_atkSlots[a.SN] = p;
        }
        st.progressFrame = info.GameFrame; st.px = a.DR; st.py = a.UR;
    }
    { // 每个实际移动路点避开快照中的拥堵单位；前线规划仍忽略短暂占位。
        Point waypoint;
        if (atkRouteStep((int)(a.DR/BLOCKSIDELENGTH),(int)(a.UR/BLOCKSIDELENGTH),
            (int)(gx/BLOCKSIDELENGTH),(int)(gy/BLOCKSIDELENGTH),5,0,waypoint,true,true)) {
            const int sx=int(a.DR/BLOCKSIDELENGTH),sy=int(a.UR/BLOCKSIDELENGTH);
            // 远路点只用于畅通直路；弯道按第一步执行，防止引擎抄近路穿过护卫。
            if(!atkStraightOpen(sx,sy,waypoint.x,waypoint.y,true))
                atkRouteStep(sx,sy,int(gx/BLOCKSIDELENGTH),int(gy/BLOCKSIDELENGTH),1,0,waypoint,true,true);
            gx=atkBD(waypoint.x); gy=atkBD(waypoint.y);
            if(waypoint.y==sy)gy=a.UR;
            if(waypoint.x==sx)gx=a.DR;
        }
        else {
            if(aiTraceEnabled()&&info.GameFrame-m_atkRouteTraceFrame>=250){
                m_atkRouteTraceFrame=info.GameFrame;
                const int x=int(a.DR/BLOCKSIDELENGTH),y=int(a.UR/BLOCKSIDELENGTH);
                std::cout<<"[MOVE_BLOCK] f="<<info.GameFrame<<" sn="<<a.SN<<" sort="<<a.Sort
                    <<" pos="<<x<<","<<y<<" goal="<<int(gx/BLOCKSIDELENGTH)<<","<<int(gy/BLOCKSIDELENGTH)<<" cells=";
                for(int dx=-1;dx<=1;++dx)for(int dy=-1;dy<=1;++dy)
                    std::cout<<((x+dx>=0&&y+dy>=0&&x+dx<100&&y+dy<100)?m_map[x+dx][y+dy]:-1)<<",";
                std::cout<<std::endl;
            }
            return false;
        }
    }
    self->HumanMove(a.SN, gx, gy);
    st.orderFrame = info.GameFrame; st.gx = gx; st.gy = gy;
    st.destinationX=destinationX;st.destinationY=destinationY;
    m_issued.insert(a.SN); ++orders; return true;
}
static bool atkAction(UsrAI* self, const tagInfo& info, const tagArmy& a,
                      int target, int& orders)
{
    if (orders >= ATK_MAX_ORDER || m_issued.count(a.SN) || target < 0) return false;
    if (a.Sort != AT_PRIEST && target == m_siegeSN) return false; // 工厂绝不能打掉
    if(!defenseStoneSafe(info,a,target))return false;
    if (a.WorkObjectSN == target && a.NowState != HUMAN_STATE_IDLE) return false;
    auto last = m_atkActions.find(a.SN);
    if (last != m_atkActions.end() && info.GameFrame - last->second < 50) return false;
    self->HumanAction(a.SN, target);
    m_atkActions[a.SN] = info.GameFrame;
    m_issued.insert(a.SN); ++orders; return true;
}
static void atkLayout(const tagInfo& info, const tagArmy& priest)
{
    const int x = m_atkFrontX, y = m_atkFrontY;
    bool connected[10000];atkFormationArea(x,y,connected);
    if (m_atkSlotX != x || m_atkSlotY != y) {
        m_atkSlots.clear(); m_atkSlotX = x; m_atkSlotY = y;
    }
    std::set<int> alive;
    std::vector<const tagArmy*> army;
    for (const auto& a : info.armies) if (atkFighter(a)&&armyTravelPossible(info,a)) {
        alive.insert(a.SN); army.push_back(&a);
    }
    alive.insert(priest.SN);
    for (auto it = m_atkSlots.begin(); it != m_atkSlots.end(); )
        if (!alive.count(it->first) || !atkOpen(it->second.x, it->second.y,true)
            || !connected[it->second.x*100+it->second.y])
            it = m_atkSlots.erase(it);
        else ++it;
    if (!m_atkSlots.count(priest.SN)) {
        const Point rear=atkRear(x,y);Point best(x,y);double score=1e18;
        for(int sx=std::max(1,x-8);sx<=std::min(98,x+8);++sx)
            for(int sy=std::max(1,y-8);sy<=std::min(98,y+8);++sy)
                if(connected[sx*100+sy]&&atkOpen(sx,sy,true)){
                    const double d=std::hypot(sx-rear.x,sy-rear.y);
                    if(d<score){score=d;best=Point(sx,sy);}
                }
        m_atkSlots[priest.SN]=best;
    }
    if(priest.Blood<priest.MaxBlood*3/5){
        const Point rear=m_atkSlots[priest.SN];
        auto towerUnsafe=[&](int sx,int sy){
            for(const auto& b:info.enemy_buildings)if(b.Type==BUILDING_ARROWTOWER&&b.Blood>0
                &&std::hypot(sx-b.BlockDR,sy-b.BlockUR)<=DIS_ARROWTOWER+2)return true;
            return false;
        };
        if(towerUnsafe(rear.x,rear.y)){
            Point safe=rear;double best=1e18;
            for(int sx=std::max(1,x-10);sx<=std::min(98,x+10);++sx)
                for(int sy=std::max(1,y-10);sy<=std::min(98,y+10);++sy)
                    if(connected[sx*100+sy]&&atkOpen(sx,sy,true)&&!towerUnsafe(sx,sy)){
                        const double score=std::hypot(sx-rear.x,sy-rear.y)
                            +0.1*std::hypot(atkBD(sx)-priest.DR,atkBD(sy)-priest.UR)/BLOCKSIDELENGTH;
                        if(score<best){best=score;safe=Point(sx,sy);}
                    }
            m_atkSlots[priest.SN]=safe;
        }
    }
    // 编号稳定，不受快照向量随机打乱影响；已有位置不因增援加入而漂移。
    std::sort(army.begin(), army.end(), [](const tagArmy* a, const tagArmy* b) {
        if (atkRanged(*a) != atkRanged(*b)) return !atkRanged(*a);
        return a->SN < b->SN;
    });
    double dx, dy; atkDirection(x, y, dx, dy);
    const int radius = army.size() > 24 ? 3 : 2;
    for (const tagArmy* a : army) {
        if (m_atkSlots.count(a->SN)) continue;
        bool found = false; Point best; double score = -1e18;
        for (int r = radius; r <= radius + 3 && (!found || atkRanged(*a)); ++r) {
            for (int sx = x - r; sx <= x + r; ++sx)
                for (int sy = y - r; sy <= y + r; ++sy) {
                    if (!atkOpen(sx, sy,true)||!connected[sx*100+sy]) continue;
                    bool taken = false;
                    for (const auto& s : m_atkSlots)
                        if (s.second.x == sx && s.second.y == sy) { taken = true; break; }
                    if (taken) continue;
                    const double forward = (sx - x) * dx + (sy - y) * dy;
                    // 弓兵横向铺开成浅阵线，而非先填满最后排，保证多数兵能同时射击。
                    const double value = (atkRanged(*a) ? -std::abs(forward+1.0) : forward) * 10
                        - std::hypot(sx - x, sy - y)*0.5;
                    if (value > score) { score = value; best = Point(sx, sy); found = true; }
                }
        }
        if (found) m_atkSlots[a->SN] = best;
    }
}
static bool atkReady(const tagInfo& info, const tagArmy& priest, int& escorts, int probingSN = -1)
{
    escorts = 0;
    bool all = true;
    int core = 0, formed = 0,formedComposite=0,formedFront=0;
    for (const auto& a : info.armies) if (atkFighter(a)&&armyTravelPossible(info,a)) {
        auto slot = m_atkSlots.find(a.SN);
        const bool atSlot = slot != m_atkSlots.end() && atkDist(a.DR, a.UR,
            atkBD(slot->second.x), atkBD(slot->second.y)) <= 2.0 * BLOCKSIDELENGTH;
        const bool inFormation = atSlot || (atkDist(a.DR, a.UR, priest.DR, priest.UR) <= 9.0 * BLOCKSIDELENGTH
            && atkDist(a.DR, a.UR, atkBD(m_atkFrontX), atkBD(m_atkFrontY))
                <= (m_atkPhase==ATK_RALLY ? 6.0 : 11.0) * BLOCKSIDELENGTH);
        if (m_atkCoreSquad.count(a.SN) && a.SN != probingSN) {
            ++core;
            if (inFormation) {
                ++formed;if(a.Sort==AT_COMPOSITE_BOWMAN)++formedComposite;
                if(frontlineType(a.Sort)&&a.MaxBlood>0&&a.Blood*2>=a.MaxBlood)++formedFront;
            }
            else all = false;
        }
        if (atkDist(a.DR, a.UR, priest.DR, priest.UR) <= 9 * BLOCKSIDELENGTH) ++escorts;
    }
    auto slot = m_atkSlots.find(priest.SN);
    bool priestReady = slot != m_atkSlots.end()
        && atkDist(priest.DR, priest.UR, atkBD(slot->second.x), atkBD(slot->second.y))
            <= 3.0 * BLOCKSIDELENGTH;
    // 行军按受护卫保护的后排区域判断，避免碰撞停在槽位外几格永远不推进。
    if(!priestReady && m_atkPhase!=ATK_RALLY && escorts>=6
        && atkDist(priest.DR,priest.UR,atkBD(m_atkFrontX),atkBD(m_atkFrontY))<=10*BLOCKSIDELENGTH){
        double dx,dy;atkDirection(m_atkFrontX,m_atkFrontY,dx,dy);
        bool clearRoute=true;
        for(const auto& e:info.enemy_armies)if(e.Blood>0
            &&atkDist(e.DR,e.UR,priest.DR,priest.UR)<=15*BLOCKSIDELENGTH)clearRoute=false;
        for(const auto& b:info.enemy_buildings)if(b.Type==BUILDING_ARROWTOWER&&b.Blood>0
            &&atkDist(atkBD(b.BlockDR),atkBD(b.BlockUR),atkBD(m_atkFrontX),atkBD(m_atkFrontY))
                <=14*BLOCKSIDELENGTH)clearRoute=false;
        priestReady=(priest.DR-atkBD(m_atkFrontX))*dx+(priest.UR-atkBD(m_atkFrontY))*dy
            <=(clearRoute?3.0:1.5)*BLOCKSIDELENGTH;
    }
    // 首次集结仍须全体核心到位；安全区域行军允许少数落队者追赶。
    const int minimum=info.GameFrame>=ATK_LAST_FRAME?ATK_LAST_ARMY:ATK_MAIN_ARMY;
    const bool groupReady = (m_atkPhase==ATK_RALLY ? formed>=std::min(minimum,atkArmyCount(info))
        : all || (m_atkPhase == ATK_PROBE && formed >= 6 && formed * 2 >= core)
            || (m_atkPhase == ATK_FIGHT && formed >= 12 && formed * 2 >= core));
    const bool deployed = m_atkPhase != ATK_RALLY || formed >= std::min(minimum,atkArmyCount(info));
    const bool complete=m_atkPhase!=ATK_RALLY
        ||(formedComposite>=COUNTER_COMPOSITE_TARGET&&formedFront>=COUNTER_FRONT_TARGET);
    return groupReady && deployed && complete && priestReady && escorts >= std::min(6, atkArmyCount(info));
}
static void atkPickPoints(const tagArmy& priest)
{
    m_atkTargetX = m_siegeX >= 0 ? m_siegeX : m_enemyBaseX;
    m_atkTargetY = m_siegeY >= 0 ? m_siegeY : m_enemyBaseY;
    if (m_atkTargetX < 0 || m_atkTargetY < 0) {
        m_atkTargetX = m_centerX < MAP_L / 2 ? MAP_L - 12 : 12;
        m_atkTargetY = m_centerY < MAP_U / 2 ? MAP_U - 12 : 12;
    }
    const int hx = m_centerX >= 0 ? m_centerX : MAP_L / 2;
    const int hy = m_centerY >= 0 ? m_centerY : MAP_U / 2;
    int x = (hx + m_atkTargetX) / 2, y = (hy + m_atkTargetY) / 2;
    // 优先使用探路首次接触位置，向基地退十二格；其次用阵亡点。
    const int cx = m_scoutContactX >= 0 ? m_scoutContactX : m_scoutDeathX;
    const int cy = m_scoutContactY >= 0 ? m_scoutContactY : m_scoutDeathY;
    if (cx >= 0 && cy >= 0 && std::hypot(hx - cx, hy - cy) > 25) {
        const double len = std::hypot(hx - cx, hy - cy);
        const double pull = std::min(12.0, len);
        if (len > 0.01) {
            x = cx + (int)std::lround((hx - cx) * pull / len);
            y = cy + (int)std::lround((hy - cy) * pull / len);
        }
    }
    Point p;
    // 初次集结先选基地外二十格内的已探明位置，避免各兵种先各自穿越半张地图。
    const double homeDistance=std::hypot(x-hx,y-hy);
    if(homeDistance>20){x=hx+(int)std::lround((x-hx)*20/homeDistance);y=hy+(int)std::lround((y-hy)*20/homeDistance);}
    if (atkFindOpen(x, y, 8, p, true)) { x = p.x; y = p.y; }
    else { x = hx; y = hy; }
    // 集结点必须从祭司所在已知陆地可达；不能只因隔河那格是空地就选它。
    // 复用完整寻路，目标不通时取同一连通区域内最接近目标的位置。
    const int px=int(priest.DR/BLOCKSIDELENGTH),py=int(priest.UR/BLOCKSIDELENGTH);
    if(atkRouteStep(px,py,x,y,10000,0,p,true)){x=p.x;y=p.y;}
    else{x=px;y=py;}
    m_atkRallyX = m_atkFrontX = x; m_atkRallyY = m_atkFrontY = y;
    m_atkTrail.clear(); m_atkTrail.push_back(Point(x,y));
}
static bool atkRouteStep(int x, int y, int tx, int ty, int step, int stopRadius, Point& out,bool known,bool avoidUnits)
{
    if (x < 0 || y < 0 || x >= std::min(100,int(MAP_L)) || y >= std::min(100,int(MAP_U)) || step <= 0) return false;
    if (std::hypot(tx - x, ty - y) <= stopRadius) return false;
    // 引擎将其它单位也计入寻路障碍；实际移动不能把拥挤的兵当空地穿过去。
    auto open=[&](int px,int py){return atkOpen(px,py,known)
        && (!avoidUnits || m_map[px][py]!=200 || (px==x&&py==y));};
    // 短步前压也必须沿可达路径：直线附近的空地可能仍在原地或隔着水域。
    int parent[10000]; std::fill(parent, parent + 10000, -1);
    int queue[10000], head = 0, tail = 0;
    const int start = x * 100 + y;
    parent[start] = start; queue[tail++] = start;
    int goal = start;
    double best = std::hypot(tx - x, ty - y);
    while (head < tail) {
        const int current = queue[head++], cx = current / 100, cy = current % 100;
        const double distance = std::hypot(tx - cx, ty - cy);
        if (distance < best) { best = distance; goal = current; }
        if (distance <= stopRadius) { goal = current; break; }
        // 朝终点的相邻格优先，空旷处保持对角行军，遇障碍则绕行。
        std::vector<Point> next;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy) {
                if ((!dx && !dy) || !open(cx + dx, cy + dy)) continue;
                if (dx && dy && (!open(cx + dx, cy) || !open(cx, cy + dy))) continue;
                const int index = (cx + dx) * 100 + cy + dy;
                if (parent[index] < 0) next.push_back(Point(cx + dx, cy + dy));
            }
        std::sort(next.begin(), next.end(), [tx,ty](const Point& a, const Point& b) {
            return std::hypot(tx - a.x, ty - a.y)
                < std::hypot(tx - b.x, ty - b.y);
        });
        for (const auto& p : next) {
            const int index = p.x * 100 + p.y;
            if (parent[index] >= 0) continue;
            parent[index] = current; queue[tail++] = index;
        }
    }
    if (goal == start) return false;
    std::vector<int> path;
    for (int at = goal; at != start; at = parent[at]) path.push_back(at);
    const int selected = path[path.size() - std::min((int)path.size(), step)];
    out = Point(selected / 100, selected % 100); return true;
}
static bool atkPathStep(int x, int y, int step, int stopRadius, Point& out)
{
    return atkRouteStep(x,y,m_atkTargetX,m_atkTargetY,step,stopRadius,out);
}
static bool atkAdvance(int step)
{
    Point p;
    if (!atkPathStep(m_atkFrontX, m_atkFrontY, step, 5, p)) return false;
    if (m_atkTrail.empty()) m_atkTrail.push_back(Point(m_atkFrontX,m_atkFrontY));
    m_atkTrail.push_back(p);
    m_atkFrontX = p.x; m_atkFrontY = p.y; return true;
}
// 只比较当前接战区的兵力，远处增援不能抵消眼前的敌营压力。
static bool atkEnemyEngaged(const tagInfo& info,const tagArmy& enemy,const tagArmy& priest,
                            bool potential=false)
{
    if(enemy.Blood<=0)return false;
    const bool ranged=atkRanged(enemy)||enemy.Sort==AT_STONE_THROWER;
    if(enemy.WorkObjectSN==priest.SN
        &&atkDist(enemy.DR,enemy.UR,priest.DR,priest.UR)<=14*BLOCKSIDELENGTH)return true;
    for(const auto& a:info.armies)if(atkFighter(a)){
        const double distance=atkDist(enemy.DR,enemy.UR,a.DR,a.UR)/BLOCKSIDELENGTH;
        if((enemy.WorkObjectSN==a.SN && distance<=(ranged?defenseFireRange(enemy)+5:6))
            ||(!ranged&&distance<=3.5)
            ||(potential&&distance<=(ranged?defenseFireRange(enemy)+0.5:3.5))
            ||(a.WorkObjectSN==enemy.SN
                &&distance<=(atkRanged(a)?defenseFireRange(a)+0.5:3.5)))return true;
    }
    return false;
}
static bool atkOutnumbered(const tagInfo& info,const tagArmy& priest)
{
    double own=0,enemy=0;int foes=0,closeMelee=0,front=0;
    for(const auto& a:info.armies)if(atkFighter(a)
        &&atkDist(a.DR,a.UR,atkBD(m_atkFrontX),atkBD(m_atkFrontY))<=10*BLOCKSIDELENGTH){
        const double health=std::min(1.0,double(a.Blood)/std::max(1,a.MaxBlood));
        own+=(atkRanged(a)?1.2:1.0)*(0.4+0.6*health);
        if(!atkRanged(a)&&a.Blood>=a.MaxBlood/2)++front;
    }
    for(const auto& e:info.enemy_armies)if(e.Blood>0
        &&(atkDist(e.DR,e.UR,atkBD(m_atkFrontX),atkBD(m_atkFrontY))<=12*BLOCKSIDELENGTH
            ||atkDist(e.DR,e.UR,priest.DR,priest.UR)<=10*BLOCKSIDELENGTH)){
        // 回防中的守军仍可能出现在视野里；只把正在接近攻击或已能攻击主力的敌人计入压力。
        if(!atkEnemyEngaged(info,e,priest,true))continue;
        ++foes;
        const double health=std::min(1.0,double(e.Blood)/std::max(1,e.MaxBlood));
        enemy+=(e.Sort==AT_STONE_THROWER?2.0:(atkRanged(e)?1.3:1.0))*(0.4+0.6*health);
        if(!atkRanged(e)&&e.Sort!=AT_PRIEST
            &&atkDist(e.DR,e.UR,atkBD(m_atkFrontX),atkBD(m_atkFrontY))<=7*BLOCKSIDELENGTH)++closeMelee;
    }
    return foes>=8 && (enemy>own*1.25 || (front<=1 && closeMelee>=6));
}
static bool atkPullback()
{
    const Point current(m_atkFrontX,m_atkFrontY);
    for(int i=int(m_atkTrail.size())-2;i>=0;--i){
        const Point p=m_atkTrail[i];
        const double distance=std::hypot(p.x-current.x,p.y-current.y);
        if(distance<4 || distance>8 || !atkOpen(p.x,p.y,true))continue;
        Point check;
        if(!atkRouteStep(current.x,current.y,p.x,p.y,8,0,check,true)
            ||check.x!=p.x||check.y!=p.y)continue;
        m_atkFrontX=p.x;m_atkFrontY=p.y;
        m_atkTrail.resize(i+1);m_atkSlotX=m_atkSlotY=-1;
        m_assaultSN=-1;m_assaultStage=0;
        return true;
    }
    // 没有近处旧路点时，只后退六个路径步，保留前线位置等增援。
    if(m_siegeSN>=0 && m_siegeX>=0 && std::hypot(current.x-m_siegeX,current.y-m_siegeY)<27){
        Point escape;
        if(atkRouteStep(current.x,current.y,m_centerX,m_centerY,6,4,escape,true)
            &&atkOpen(escape.x,escape.y,true)
            &&std::hypot(escape.x-current.x,escape.y-current.y)>=4){
            m_atkFrontX=escape.x;m_atkFrontY=escape.y;
            m_atkTrail.clear();m_atkTrail.push_back(escape);
            m_atkSlotX=m_atkSlotY=-1;m_assaultSN=-1;m_assaultStage=0;
            return true;
        }
    }
    return false;
}
static double atkEngageDistance(const tagArmy& a,bool pulling)
{
    if(atkRanged(a))return defenseFireRange(a)-0.25;
    return pulling?2.5:3.5;
}

// 炮弹落点在发射时固定；拿不到炮弹快照时，用瞄准者及其邻近区域做预防性侧移。
static bool atkDodgeStone(UsrAI* self,const tagInfo& info,const tagArmy& archer,
                         const tagArmy& priest,int& orders)
{
    if(!atkRanged(archer))return false;
    const auto last=m_atkDodgeFrames.find(archer.SN);
    if(last!=m_atkDodgeFrames.end()&&info.GameFrame-last->second<75
        &&archer.NowState==HUMAN_STATE_WALKING)return true;
    if(last!=m_atkDodgeFrames.end()&&info.GameFrame-last->second<50)return false;
    for(const auto& e:info.enemy_armies)if(e.Blood>0&&e.Sort==AT_STONE_THROWER){
        const tagArmy* aim=nullptr;
        for(const auto& a:info.armies)if(a.Blood>0&&a.SN==e.WorkObjectSN){aim=&a;break;}
        if(!aim || atkDist(e.DR,e.UR,aim->DR,aim->UR)>11*BLOCKSIDELENGTH
            ||atkDist(archer.DR,archer.UR,aim->DR,aim->UR)>1.2*BLOCKSIDELENGTH)continue;
        const double dx=aim->DR-e.DR,dy=aim->UR-e.UR,len=std::hypot(dx,dy);
        if(len<0.1)continue;
        const int sx=int(archer.DR/BLOCKSIDELENGTH),sy=int(archer.UR/BLOCKSIDELENGTH);
        bool found=false;Point best;double score=-1e18;
        double fx,fy;atkDirection(m_atkFrontX,m_atkFrontY,fx,fy);
        for(int sign:{-1,1})for(int side:{2,3,4}){
            const int tx=sx-int(std::lround(sign*dy/len*side));
            const int ty=sy+int(std::lround(sign*dx/len*side));
            Point p;
            if(!atkRouteStep(sx,sy,tx,ty,4,0,p)||!atkOpen(p.x,p.y,true))continue;
            const double px=atkBD(p.x),py=atkBD(p.y);
            const double clearance=atkDist(px,py,aim->DR,aim->UR)/BLOCKSIDELENGTH;
            if(clearance<1.5 || atkDist(px,py,priest.DR,priest.UR)>10*BLOCKSIDELENGTH
                ||(px-atkBD(m_atkFrontX))*fx+(py-atkBD(m_atkFrontY))*fy>2*BLOCKSIDELENGTH)continue;
            bool tower=false;int crowd=0;
            for(const auto& b:info.enemy_buildings)if(b.Type==BUILDING_ARROWTOWER&&b.Blood>0
                &&atkDist(px,py,atkBD(b.BlockDR),atkBD(b.BlockUR))<8*BLOCKSIDELENGTH)tower=true;
            if(tower)continue;
            bool reserved=false;
            for(const auto& move:m_atkMoves)if(move.first!=archer.SN){
                const auto escape=m_atkDodgeFrames.find(move.first);
                if(escape!=m_atkDodgeFrames.end()&&info.GameFrame-escape->second<75
                    &&atkDist(px,py,move.second.gx,move.second.gy)<1.2*BLOCKSIDELENGTH)reserved=true;
            }
            if(reserved)continue;
            for(const auto& a:info.armies)if(a.SN!=archer.SN&&a.Blood>0
                &&atkDist(px,py,a.DR,a.UR)<1.2*BLOCKSIDELENGTH)++crowd;
            const double value=clearance*4-crowd*10-atkDist(px,py,archer.DR,archer.UR)/BLOCKSIDELENGTH;
            if(value>score){score=value;best=p;found=true;}
        }
        if(found&&atkMove(self,info,archer,atkBD(best.x),atkBD(best.y),orders,true)){
            m_atkDodgeFrames[archer.SN]=info.GameFrame;
            return true;
        }
    }
    return false;
}

// 收尾路径使用地图的完整合法范围（0..99），边缘陆地同样可走。
static bool atkEscortOpen(int x,int y)
{
    return x>=0&&y>=0&&x<std::min(100,int(MAP_L))&&y<std::min(100,int(MAP_U))
        &&(m_map[x][y]==0||m_map[x][y]==200);
}
static bool atkEscortPath(const tagArmy& unit,const Point& target,bool factory,std::vector<int>& path,bool avoidUnits=false)
{
    path.clear();const int sx=int(unit.DR/BLOCKSIDELENGTH),sy=int(unit.UR/BLOCKSIDELENGTH);
    if(!atkEscortOpen(sx,sy))return false;
    auto open=[&](int x,int y){return atkEscortOpen(x,y)
        &&(!avoidUnits || m_map[x][y]!=200 || (x==sx&&y==sy));};
    int parent[10000];std::fill(parent,parent+10000,-1);
    int queue[10000],head=0,tail=0;const int start=sx*100+sy;
    parent[start]=start;queue[tail++]=start;int goal=-1;
    while(head<tail){
        const int at=queue[head++],x=at/100,y=at%100;
        const bool reached=factory
            ?(((x==m_siegeX-1||x==m_siegeX+3)&&y>=m_siegeY&&y<m_siegeY+3)
                ||((y==m_siegeY-1||y==m_siegeY+3)&&x>=m_siegeX&&x<m_siegeX+3))
            :(x==target.x&&y==target.y);
        if(reached){goal=at;break;}
        for(int dx=-1;dx<=1;++dx)for(int dy=-1;dy<=1;++dy){
            const int nx=x+dx,ny=y+dy;
            if((!dx&&!dy)||!open(nx,ny))continue;
            if(dx&&dy&&(!open(x+dx,y)||!open(x,y+dy)))continue;
            const int next=nx*100+ny;
            if(parent[next]<0){parent[next]=at;queue[tail++]=next;}
        }
    }
    if(goal<0)return false;
    for(int node=goal;node!=start;node=parent[node])path.push_back(node);
    std::reverse(path.begin(),path.end());return true;
}
static bool atkFactoryEscortStep(const tagArmy& priest,Point& step,bool& adjacent)
{
    std::vector<int> path;
    if(!atkEscortPath(priest,Point(m_siegeX,m_siegeY),true,path,true))return false;
    adjacent=path.empty();step=Point(int(priest.DR/BLOCKSIDELENGTH),int(priest.UR/BLOCKSIDELENGTH));
    if(!adjacent)step=Point(path.front()/100,path.front()%100);
    return true;
}
static bool atkEscortMove(UsrAI* self,const tagInfo& info,const tagArmy& unit,const Point& goal,int& orders,double arrival=0.25)
{
    if(orders>=ATK_MAX_ORDER||m_issued.count(unit.SN)
        ||atkDist(unit.DR,unit.UR,atkBD(goal.x),atkBD(goal.y))<=arrival*BLOCKSIDELENGTH)return false;
    AtkMoveState& movement=m_atkMoves[unit.SN];
    if(movement.progressFrame<0||atkDist(movement.px,movement.py,unit.DR,unit.UR)>BLOCKSIDELENGTH){
        movement.progressFrame=info.GameFrame;movement.px=unit.DR;movement.py=unit.UR;
    }
    if(info.GameFrame-movement.orderFrame<15)return false;
    const bool same=atkDist(movement.destinationX,movement.destinationY,atkBD(goal.x),atkBD(goal.y))<=0.25*BLOCKSIDELENGTH;
    if(same&&unit.NowState==HUMAN_STATE_WALKING&&info.GameFrame-movement.progressFrame<250)return false;
    std::vector<int> path;
    if(!atkEscortPath(unit,goal,false,path,true))return false;
    // 逐格执行已规划路线，避免引擎对远路点重新寻路时又穿过护卫。
    const int next=path.empty()?goal.x*100+goal.y:path.front();
    double gx=atkBD(next/100),gy=atkBD(next%100);
    // 横移/纵移保持另一轴的实际位置；强行对齐格心会变成斜走并撞到旁边的兵。
    if(!path.empty()){
        const int sx=int(unit.DR/BLOCKSIDELENGTH),sy=int(unit.UR/BLOCKSIDELENGTH);
        if(next%100==sy)gy=unit.UR;
        if(next/100==sx)gx=unit.DR;
    }
    self->HumanMove(unit.SN,gx,gy);
    movement.orderFrame=info.GameFrame;movement.gx=gx;movement.gy=gy;
    movement.destinationX=atkBD(goal.x);movement.destinationY=atkBD(goal.y);
    m_issued.insert(unit.SN);++orders;return true;
}
// 清场后若塔锁定祭司，沿已探明陆路退到塔射程外。
static bool atkFinishTowerSafe(const tagInfo& info,int x,int y)
{
    for(const auto& b:info.enemy_buildings)if(b.Type==BUILDING_ARROWTOWER&&b.Blood>0
        &&atkDist(atkBD(x),atkBD(y),atkBD(b.BlockDR),atkBD(b.BlockUR))
            <=(DIS_ARROWTOWER+2)*BLOCKSIDELENGTH)return false;
    return true;
}
static bool atkFinishSafePoint(const tagInfo& info,const tagArmy& priest,Point& out)
{
    const int sx=int(priest.DR/BLOCKSIDELENGTH),sy=int(priest.UR/BLOCKSIDELENGTH);
    if(!atkEscortOpen(sx,sy))return false;
    bool seen[10000]={};int queue[10000],head=0,tail=0;
    queue[tail++]=sx*100+sy;seen[sx*100+sy]=true;
    while(head<tail){
        const int at=queue[head++],x=at/100,y=at%100;
        // 撤退终点必须有空位；护卫占用格可作为让路通道，不能作为永久等待的终点。
        if(atkFinishTowerSafe(info,x,y) && (m_map[x][y]!=200 || (x==sx&&y==sy))){out=Point(x,y);return true;}
        for(int dx=-1;dx<=1;++dx)for(int dy=-1;dy<=1;++dy){
            const int nx=x+dx,ny=y+dy;
            if((!dx&&!dy)||!atkEscortOpen(nx,ny))continue;
            if(dx&&dy&&(!atkEscortOpen(x+dx,y)||!atkEscortOpen(x,y+dy)))continue;
            const int index=nx*100+ny;
            if(!seen[index]){seen[index]=true;queue[tail++]=index;}
        }
    }
    return false;
}
// 清场后只有一套命令：部队前进拆沿途塔，祭司沿空路接近工厂。
static bool atkFinishEscort(UsrAI* self,const tagInfo& info,const tagArmy& priest,int& orders)
{
    if(m_atkPhase==ATK_RALLY||m_siegeSN<0||m_siegeX<0)return false;
    for(const auto& e:info.enemy_armies)if(e.Blood>0)return false;
    std::vector<const tagArmy*> soldiers;
    for(const auto& a:info.armies)if(atkFighter(a)&&armyTravelPossible(info,a))soldiers.push_back(&a);
    std::sort(soldiers.begin(),soldiers.end(),[](const tagArmy* a,const tagArmy* b){
        const int af=atkLastOrderFrame(a->SN),bf=atkLastOrderFrame(b->SN);
        return af==bf?a->SN<b->SN:af<bf;
    });
    const Point position(int(priest.DR/BLOCKSIDELENGTH),int(priest.UR/BLOCKSIDELENGTH));
    const bool casting=m_siegeSN>=0&&priest.WorkObjectSN==m_siegeSN&&priest.ConvertCooldown<=0;
    bool locked=false;
    for(const auto& b:info.enemy_buildings)if(b.Type==BUILDING_ARROWTOWER&&b.Blood>0
        &&b.Project==priest.SN)locked=true;
    if(locked&&!casting)m_atkFinishWithdraw=true;
    if(casting||atkFinishTowerSafe(info,position.x,position.y))m_atkFinishWithdraw=false;
    const bool withdraw=m_atkFinishWithdraw;
    m_atkHolding=false;m_assaultSN=-1;m_assaultStage=0;
    Point step=position,forward=position;std::vector<int> corridor;
    bool adjacent=false,route=false;
    if(withdraw){
        route=atkFinishSafePoint(info,priest,step);
        if(route)atkEscortPath(priest,step,false,corridor);
    }else{
        atkEscortPath(priest,Point(m_siegeX,m_siegeY),true,corridor);
        route=atkFactoryEscortStep(priest,step,adjacent);
    }
    if(!corridor.empty()){
        const int ahead=corridor[std::min(7,int(corridor.size())-1)];
        forward=Point(ahead/100,ahead%100);
    }
    bool covered=true;
    if(!withdraw&&route)for(const auto& b:info.enemy_buildings)if(b.Type==BUILDING_ARROWTOWER&&b.Blood>0
        &&atkDist(atkBD(step.x),atkBD(step.y),atkBD(b.BlockDR),atkBD(b.BlockUR))<=(DIS_ARROWTOWER+1)*BLOCKSIDELENGTH){
        bool held=false;
        for(const auto* a:soldiers)if(b.Project==a->SN
            &&std::hypot(int(a->DR/BLOCKSIDELENGTH)-b.BlockDR,int(a->UR/BLOCKSIDELENGTH)-b.BlockUR)<=int(DIS_ARROWTOWER))held=true;
        if(!held)covered=false;
    }
    if(!casting&&route){
        if(withdraw)atkEscortMove(self,info,priest,step,orders);
        else if(covered){
            if(adjacent&&priest.ConvertCooldown<=0){
                if(atkAction(self,info,priest,m_siegeSN,orders))m_convertStartFrame=info.GameFrame;
                m_atkPhase=ATK_CONVERT;
            }else if(!adjacent)atkEscortMove(self,info,priest,step,orders);
        }
    }
    // 掩护消失时取消正在执行的旧移动；已开始转换则保持动作。
    if(!casting&&!withdraw&&!covered&&priest.NowState==HUMAN_STATE_WALKING
        &&orders<ATK_MAX_ORDER&&!m_issued.count(priest.SN)){
        self->HumanMove(priest.SN,priest.DR,priest.UR);m_issued.insert(priest.SN);++orders;
        m_atkMoves[priest.SN].orderFrame=info.GameFrame;
    }
    int firing=0;
    for(const auto* a:soldiers){
        if(orders>=ATK_MAX_ORDER)break;
        if(m_issued.count(a->SN))continue;
        const tagBuilding* target=nullptr;double closest=1e18;
        // 保留正在吸火的兵，其他部队一同进射程拆塔，不再围着祭司布阵。
        for(const auto& b:info.enemy_buildings)if(b.Type==BUILDING_ARROWTOWER&&b.Blood>0){
            const bool holding=b.Project==a->SN
                &&std::hypot(int(a->DR/BLOCKSIDELENGTH)-b.BlockDR,int(a->UR/BLOCKSIDELENGTH)-b.BlockUR)<=int(DIS_ARROWTOWER);
            const bool alongRoute=atkDist(priest.DR,priest.UR,atkBD(b.BlockDR),atkBD(b.BlockUR))<=16*BLOCKSIDELENGTH
                ||atkDist(atkBD(forward.x),atkBD(forward.y),atkBD(b.BlockDR),atkBD(b.BlockUR))<=10*BLOCKSIDELENGTH;
            if(!holding&&!alongRoute)continue;
            const double d=holding?-1:atkDist(a->DR,a->UR,atkBD(b.BlockDR),atkBD(b.BlockUR));
            if(d<closest){closest=d;target=&b;}
        }
        if(target){
            const double range=std::max(1.0,defenseFireRange(*a)-0.25);
            if(atkDist(a->DR,a->UR,atkBD(target->BlockDR),atkBD(target->BlockUR))<=range*BLOCKSIDELENGTH){
                if(a->Sort==AT_STONE_THROWER&&!defenseStoneSafe(info,*a,target->SN)){
                    if(a->WorkObjectSN>0){self->HumanMove(a->SN,a->DR,a->UR);m_issued.insert(a->SN);++orders;}
                }else {atkAction(self,info,*a,target->SN,orders);++firing;}
                continue;
            }
            Point approach;
            if(atkRouteStep(int(a->DR/BLOCKSIDELENGTH),int(a->UR/BLOCKSIDELENGTH),
                target->BlockDR,target->BlockUR,5,int(range),approach,true,true))
                atkEscortMove(self,info,*a,approach,orders);
            continue;
        }
        if(!corridor.empty()){
            const int ahead=corridor[std::min(3+a->SN%5,int(corridor.size())-1)];
            Point advance(ahead/100,ahead%100),goal;
            // 通路上的兵向前或侧方让路；不叫回祭司周围的旧槽位。
            double best=1e18;bool found=false;
            for(int dx=-2;dx<=2;++dx)for(int dy=-2;dy<=2;++dy){
                const int x=advance.x+dx,y=advance.y+dy;
                if(!atkEscortOpen(x,y)||m_map[x][y]==200||std::find(corridor.begin(),corridor.end(),x*100+y)!=corridor.end())continue;
                const double d=std::hypot(dx,dy)+0.05*atkDist(a->DR,a->UR,atkBD(x),atkBD(y))/BLOCKSIDELENGTH;
                if(d<best){best=d;goal=Point(x,y);found=true;}
            }
            if(found)atkEscortMove(self,info,*a,goal,orders);
            else atkEscortMove(self,info,*a,advance,orders);
        }
    }
    if(m_atkTrail.empty()||m_atkTrail.back().x!=position.x||m_atkTrail.back().y!=position.y)m_atkTrail.push_back(position);
    m_atkFrontX=position.x;m_atkFrontY=position.y;m_atkSlotX=m_atkSlotY=-1;
    if(aiTraceEnabled()&&info.GameFrame-m_atkDbgFrame>=250){
        m_atkDbgFrame=info.GameFrame;
        std::cout<<"[ATK_FINISH] f="<<info.GameFrame<<" army="<<soldiers.size()<<" waiting=0"
            <<" withdraw="<<withdraw<<" route="<<route<<" covered="<<covered<<" adjacent="<<adjacent<<" casting="<<casting
            <<" priest="<<priest.Blood<<":"<<position.x<<","<<position.y<<" goal="<<step.x<<","<<step.y
            <<" factory="<<m_siegeSN<<" firing="<<firing<<std::endl;
    }
    return true;
}
static void attackPhase(UsrAI* self, const tagInfo& info)
{
    const int f = info.GameFrame;
    if (m_atkLastFrame >= 0 && f < m_atkLastFrame) {
        atkReset(-750); m_atkHomeQuietSince = -1;
    }
    m_atkLastFrame = f;
    const tagArmy* priest = nullptr;
    for (const auto& a : info.armies)
        if (a.Sort == AT_PRIEST && a.Blood > 0) { priest = &a; break; }
    if (!priest) { if (m_atkOn) atkReset(f); return; }
    const int army = atkArmyCount(info);
    bool homeThreat = false;
    for (const auto& e : info.enemy_armies)
        if (e.Blood > 0 && atkDist(e.DR, e.UR, atkBD(m_centerX), atkBD(m_centerY))
            <= (e.Sort==AT_STONE_THROWER?28:18) * BLOCKSIDELENGTH) { homeThreat = true; break; }
    if (homeThreat) m_atkHomeQuietSince = -1;
    else if (m_atkHomeQuietSince < 0) m_atkHomeQuietSince = f;
    if (!m_atkOn) {
        if (f < ATK_MAIN_FRAME || f < m_atkRetryAfter || homeThreat
            || m_atkHomeQuietSince < 0 || f - m_atkHomeQuietSince < ATK_QUIET_FRAMES
            || info.civilizationStage < CIVILIZATION_BRONZEAGE
) return;
        if (army < (f >= ATK_LAST_FRAME ? ATK_LAST_ARMY : ATK_MAIN_ARMY)
            || !counterArmyReady(info)) return;
        std::set<int> deployable;
        for(const auto& a:info.armies) if(atkFighter(a)) {
            const int x=(int)(a.DR/BLOCKSIDELENGTH),y=(int)(a.UR/BLOCKSIDELENGTH);
            // 出生后尚卡在己方建筑占地内的单位先留营，出营、抵达阵形后自动加入。
            const bool inHomeBuilding=x>=0&&x<100&&y>=0&&y<100&&m_map[x][y]>=100&&m_map[x][y]<200
                && atkDist(a.DR,a.UR,atkBD(m_centerX),atkBD(m_centerY))<=18*BLOCKSIDELENGTH;
            if(!inHomeBuilding) deployable.insert(a.SN);
        }
        for (const auto& a : info.armies) if (atkFighter(a)
            &&(!atkCanLeave(a)||!armyTravelPossible(info,a))) deployable.erase(a.SN);
        if((int)deployable.size()<(f>=ATK_LAST_FRAME?ATK_LAST_ARMY:ATK_MAIN_ARMY)) return;
        m_atkOn = 1; m_atkPhase = ATK_RALLY;
        m_atkCoreSquad=deployable;
        m_scoutDone = 1; m_scoutUnitSN = -1; // 探路兵也纳入军队，统计/发令一致
        m_assaultSN = -1; m_assaultStage = 0; m_atkQuietSince = -1;
        atkPickPoints(*priest);
    }
    // 后续发现工厂只更新终点，不重置已经到位的集结点/前压点。
    if (m_siegeSN >= 0 && m_siegeX >= 0) {
        m_atkTargetX = m_siegeX; m_atkTargetY = m_siegeY;
    } else if (m_enemyBaseX >= 0 && m_enemyBaseY >= 0) {
        m_atkTargetX = m_enemyBaseX; m_atkTargetY = m_enemyBaseY;
    }
    int orders = 0;
    if(atkFinishEscort(self,info,*priest,orders))return;
    atkLayout(info, *priest);
    for (const auto& a : info.armies) if (atkFighter(a)) {
        // 出生口被树林/建筑封死的兵暂留营；实际集结仍需至少12名能出营的兵。
        if (!armyTravelPossible(info,a) ||(m_atkPhase == ATK_RALLY && !atkCanLeave(a))) {
            m_atkCoreSquad.erase(a.SN);continue;
        }
        auto slot = m_atkSlots.find(a.SN);
        if (slot != m_atkSlots.end()
            && atkDist(a.DR, a.UR, atkBD(slot->second.x), atkBD(slot->second.y)) <= 2 * BLOCKSIDELENGTH)
            m_atkCoreSquad.insert(a.SN);
    }
    int escorts = 0;
    bool ready = atkReady(info, *priest, escorts);
    int enemies = 0,engagedEnemies=0;
    for (const auto& e : info.enemy_armies)
        if (e.Blood > 0 && (atkDist(e.DR, e.UR, atkBD(m_atkFrontX), atkBD(m_atkFrontY))
                            <= 14 * BLOCKSIDELENGTH
            || atkDist(e.DR, e.UR, priest->DR, priest->UR) <= 12 * BLOCKSIDELENGTH)) {
            ++enemies;if(atkEnemyEngaged(info,e,*priest))++engagedEnemies;
        }
    const bool overwhelming=atkOutnumbered(info,*priest);
    const bool factoryOpportunity=atkFactoryOpportunity(info,*priest);
    const bool finishingFactory=m_siegeSN>=0&&priest->WorkObjectSN==m_siegeSN
        &&priest->ConvertCooldown<=0&&priest->Blood>=std::max(10,priest->MaxBlood/10);
    int localArmy=0,availableArmy=0;
    for(const auto& a:info.armies)if(atkFighter(a)&&armyTravelPossible(info,a)){
        ++availableArmy;
        if(atkDist(a.DR,a.UR,atkBD(m_atkFrontX),atkBD(m_atkFrontY))<=10*BLOCKSIDELENGTH)++localArmy;
    }
    if(m_atkHolding && (factoryOpportunity||finishingFactory
        ||(!overwhelming&&localArmy>=10&&f>=m_atkPullbackUntil)))m_atkHolding=false;
    if(m_atkPhase!=ATK_RALLY && m_atkPhase!=ATK_CONVERT
        &&!m_atkHolding&&!factoryOpportunity&&!finishingFactory
        &&(overwhelming||availableArmy<ATK_ABORT_ARMY)){
        m_atkHolding=true;m_atkPullbackFrame=f;m_atkPullbackUntil=f+250;m_atkAdvanceFrame=f;
        const bool moved=atkPullback();
        if(moved){atkLayout(info,*priest);ready=atkReady(info,*priest,escorts);}
        if(aiTraceEnabled())std::cout<<"[ATK_HOLD] f="<<f<<" army="<<army<<" local="<<localArmy
            <<" enemies="<<enemies<<" moved="<<moved<<" front="<<m_atkFrontX<<","<<m_atkFrontY<<std::endl;
    }
    const bool pulling=m_atkHolding||f<m_atkPullbackUntil;
    bool towerThreat = false;
    bool towerLocksPriest = false;
    for (const auto& b : info.enemy_buildings)
        if (b.Type == BUILDING_ARROWTOWER && b.Blood > 0
            && (atkDist(atkBD(b.BlockDR), atkBD(b.BlockUR), priest->DR, priest->UR)
                    <= (DIS_ARROWTOWER + 3) * BLOCKSIDELENGTH
                || atkDist(atkBD(b.BlockDR),atkBD(b.BlockUR),atkBD(m_atkFrontX),atkBD(m_atkFrontY))
                    <= 14 * BLOCKSIDELENGTH
                || std::any_of(info.armies.begin(),info.armies.end(),[&](const tagArmy& a){
                    return atkFighter(a)&&atkDist(a.DR,a.UR,atkBD(b.BlockDR),atkBD(b.BlockUR))
                        <=16*BLOCKSIDELENGTH;
                })
                || (m_siegeX >= 0 && atkDist(atkBD(b.BlockDR), atkBD(b.BlockUR),
                        atkBD(m_siegeX), atkBD(m_siegeY)) <= 12 * BLOCKSIDELENGTH)))
        {
            towerThreat = true;
            if (b.Project == priest->SN) towerLocksPriest = true;
        }
    const int contactEnemies=m_atkPhase==ATK_RALLY?enemies:engagedEnemies;
    if (contactEnemies) m_atkQuietSince = -1;
    else if (m_atkQuietSince < 0) m_atkQuietSince = f;
    const bool quiet = m_atkQuietSince >= 0 && f - m_atkQuietSince >= ATK_QUIET_FRAMES;
    if (m_atkPhase == ATK_RALLY && ready && quiet) {
        // 足够主力与祭司已经真实集结；落队者之后作为增援加入。
        m_atkCoreSquad.clear();
        for(const auto& a:info.armies)if(atkFighter(a)
            &&atkDist(a.DR,a.UR,priest->DR,priest->UR)<=9*BLOCKSIDELENGTH)m_atkCoreSquad.insert(a.SN);
        m_atkPhase = ATK_PROBE;
    }
    if (m_atkPhase != ATK_RALLY) {
        if (engagedEnemies) m_atkPhase = ATK_FIGHT;
        else if (quiet && m_atkPhase == ATK_FIGHT) m_atkPhase = ATK_PROBE;
        // 守军已清且护卫到位；未拆完的塔必须有健康友军持续吸住火力。
        bool factoryVisible = false;
        for (const auto& a : info.armies)
            if (atkFighter(a) && m_siegeX >= 0
                && atkDist(a.DR, a.UR, atkBD(m_siegeX), atkBD(m_siegeY)) <= 6 * BLOCKSIDELENGTH)
                factoryVisible = true;
        const bool towerSafe = !towerThreat || atkConversionCover(info,*priest); // 后方空闲塔不阻止已有转换机会。
        if (((ready && enemies==0 && towerSafe && factoryVisible && escorts>=ATK_FACTORY_ESCORT_MIN)||factoryOpportunity)
            &&std::none_of(info.enemy_armies.begin(),info.enemy_armies.end(),[](const tagArmy& e){return e.Blood>0;})
            && quiet && m_siegeSN >= 0
            && priest->Blood > 0 && priest->ConvertCooldown <= 0)
            {m_atkPhase = ATK_CONVERT;m_atkHolding=false;}
        if (m_atkPhase == ATK_CONVERT
            && !finishingFactory&&!factoryOpportunity
            && (enemies || !towerSafe || escorts < 3
                || priest->Blood <= 0)) m_atkPhase = ATK_FIGHT;
    }
    // 祭司最高优先：只有致命危险才中断正在进行的转化。
    int attackers = 0; double nearest = 1e18;
    for (const auto& e : info.enemy_armies) if (e.Blood > 0) {
        const double d = atkDist(e.DR, e.UR, priest->DR, priest->UR);
        nearest = std::min(nearest, d);
        if (defensePursuerNear(e,*priest)) ++attackers;
    }
    bool nearTowerFire=false;
    for(const auto& b:info.enemy_buildings)if(b.Type==BUILDING_ARROWTOWER&&b.Blood>0
        &&atkDist(priest->DR,priest->UR,atkBD(b.BlockDR),atkBD(b.BlockUR))
            <=(DIS_ARROWTOWER+1)*BLOCKSIDELENGTH)nearTowerFire=true;
    if(nearTowerFire&&atkTowerCoverAt(info,Point(int(priest->DR/BLOCKSIDELENGTH),
        int(priest->UR/BLOCKSIDELENGTH))))nearTowerFire=false;
    // 护卫足够且敌军未锁祭司时，前排正在交战不等于祭司受追杀。
    // 敌人已贴近或护卫不足仍按原低血距离撤退；实际追杀和塔火不放宽。
    const bool proximityDanger=escorts<6 || nearest<=3*BLOCKSIDELENGTH;
    bool critical = (priest->Blood < priest->MaxBlood / 4 && (attackers > 0 || (proximityDanger&&nearest < 10 * BLOCKSIDELENGTH) || towerLocksPriest))
        || (priest->Blood < priest->MaxBlood * 3 / 5 && (attackers > 0 || (proximityDanger&&nearest < 8 * BLOCKSIDELENGTH)))
        || attackers >= 2 || ((towerLocksPriest||nearTowerFire) && priest->Blood < priest->MaxBlood * 3 / 5);
    if(enemies==0&&m_siegeSN>=0&&priest->WorkObjectSN==m_siegeSN
        &&priest->ConvertCooldown<=0&&priest->Blood>=std::max(10,priest->MaxBlood/10))critical=false;
    bool casting = m_convertStartFrame >= 0 && f - m_convertStartFrame < 200;
    // Core::infoShare将建筑工作统一标成WORKING；转换工厂不能只识别ATTACKING。
    if ((priest->NowState == HUMAN_STATE_ATTACKING || priest->NowState == HUMAN_STATE_WORKING)
        && priest->ConvertCooldown <= 0) {
        if (priest->WorkObjectSN == m_siegeSN) casting = true;
        for (const auto& e : info.enemy_armies)
            if (e.Blood > 0 && e.SN == priest->WorkObjectSN) casting = true;
    }
    const Point rear = m_atkSlots[priest->SN];
    const bool convertingFactory=m_siegeSN>=0&&priest->WorkObjectSN==m_siegeSN
        &&priest->ConvertCooldown<=0;
    bool chasingConversion=false;
    for(const auto& e:info.enemy_armies)if(e.Blood>0&&e.SN==priest->WorkObjectSN){
        double dx,dy;atkDirection(m_atkFrontX,m_atkFrontY,dx,dy);
        const double forward=(priest->DR-atkBD(m_atkFrontX))*dx+(priest->UR-atkBD(m_atkFrontY))*dy;
        chasingConversion=atkDist(priest->DR,priest->UR,e.DR,e.UR)>DIS_PRIEST*BLOCKSIDELENGTH
            ||forward>2*BLOCKSIDELENGTH
            ||atkDist(priest->DR,priest->UR,atkBD(rear.x),atkBD(rear.y))>6*BLOCKSIDELENGTH
            ||(m_convertStartFrame>=0&&f-m_convertStartFrame>225);
        if(chasingConversion)m_atkConvertAvoid[e.SN]=f;
        break;
    }
    if(chasingConversion && !critical){
        // WorkObjectSN非空也可能是在追赶；离开保护区域或施法长期未完成就取消追赶。
        if(!atkMove(self,info,*priest,atkBD(rear.x),atkBD(rear.y),orders,true)
            &&orders<ATK_MAX_ORDER&&!m_issued.count(priest->SN)){
            self->HumanMove(priest->SN,priest->DR,priest->UR);
            m_issued.insert(priest->SN);++orders;
        }
        m_convertStartFrame=-1;
    }else if (critical) {
        Point retreat;
        if (atkRetreatPoint(info,*priest,rear,retreat)){
            if(aiTraceEnabled()&&f-m_atkRetreatTraceFrame>=50){
                m_atkRetreatTraceFrame=f;
                std::cout<<"[ATK_RETREAT] f="<<f<<" hp="<<priest->Blood<<" pos="
                    <<priest->DR/BLOCKSIDELENGTH<<","<<priest->UR/BLOCKSIDELENGTH
                    <<" goal="<<retreat.x<<","<<retreat.y<<" attackers="<<attackers
                    <<" escorts="<<escorts<<" pulling="<<pulling<<std::endl;
                for(const auto& a:info.armies)if(atkFighter(a)
                    &&atkDist(a.DR,a.UR,priest->DR,priest->UR)<=10*BLOCKSIDELENGTH)
                    std::cout<<"[RETREAT_GUARD] f="<<f<<" sn="<<a.SN<<" sort="<<a.Sort
                        <<" hp="<<a.Blood<<" pos="<<a.DR/BLOCKSIDELENGTH<<","<<a.UR/BLOCKSIDELENGTH
                        <<" state="<<a.NowState<<" work="<<a.WorkObjectSN<<std::endl;
            }
            if (atkMove(self, info, *priest, atkBD(retreat.x), atkBD(retreat.y), orders, true))
                m_convertStartFrame = -1;
        }
        m_atkPhase = ATK_FIGHT;
        m_assaultSN=-1;m_assaultStage=0;
    } else if (!casting) {
        if (m_atkPhase == ATK_CONVERT && escorts >= 3) {
            if (priest->ConvertCooldown <= 0
                && atkAction(self, info, *priest, m_siegeSN, orders)) m_convertStartFrame = f;
        } else {
            int target = -1, hp = -1;
            if (m_atkPhase != ATK_RALLY && !pulling && priest->ConvertCooldown <= 0 && escorts >= 3)
                for (const auto& e : info.enemy_armies)
                    if (e.Blood > hp && e.Blood > 0
                        &&(m_atkConvertAvoid.count(e.SN)==0||f-m_atkConvertAvoid[e.SN]>=250)
                        && atkDist(e.DR, e.UR, priest->DR, priest->UR) <= 10 * BLOCKSIDELENGTH) {
                        hp = e.Blood; target = e.SN;
                    }
            if (target >= 0 && atkAction(self, info, *priest, target, orders)) m_convertStartFrame = f;
            else {
                bool healing = false;
                for (const auto& a : info.armies)
                    if (a.SN == priest->WorkObjectSN && a.SN != priest->SN) healing = true;
                atkMove(self, info, *priest, atkBD(rear.x), atkBD(rear.y), orders, healing);
            }
        }
    }
    // 突击者有独立的出击/撤回状态。受攻击、敌人靠近或超时就撤回。
    const tagArmy* bait = nullptr;
    for (const auto& a : info.armies) if (atkFighter(a) && a.SN == m_assaultSN) bait = &a;
    if (!bait) { m_assaultSN = -1; m_assaultStage = 0; }
    if (m_atkPhase == ATK_PROBE && ready && quiet && !pulling && !bait) {
        double best = 1e18;
        for (const auto& a : info.armies) if (atkFighter(a) && !atkRanged(a)
            &&a.Sort!=AT_STONE_THROWER&&a.Blood>=a.MaxBlood/2
            &&atkDist(a.DR,a.UR,atkBD(m_atkFrontX),atkBD(m_atkFrontY))<=9*BLOCKSIDELENGTH) {
            const double d = atkDist(a.DR, a.UR, atkBD(m_atkTargetX), atkBD(m_atkTargetY));
            if (d < best) { best = d; bait = &a; }
        }
        if (!bait) {
            double best=1e18;
            for (const auto& a : info.armies) if (atkFighter(a)&&a.Sort!=AT_STONE_THROWER
                &&atkDist(a.DR,a.UR,atkBD(m_atkFrontX),atkBD(m_atkFrontY))<=9*BLOCKSIDELENGTH){
                const double d=atkDist(a.DR,a.UR,atkBD(m_atkTargetX),atkBD(m_atkTargetY));
                if(d<best){best=d;bait=&a;}
            }
        }
        if (bait) {
            m_assaultSN = bait->SN; m_assaultStage = 1; m_assaultFrame = f;
            m_assaultPushed = false;
            m_assaultBlood = bait->Blood; m_assaultPX = bait->DR; m_assaultPY = bait->UR;
        }
    }
    if (bait && m_atkPhase != ATK_RALLY && m_atkPhase != ATK_CONVERT) {
        bool danger = bait->Blood < m_assaultBlood;
        for (const auto& e : info.enemy_armies) if (e.Blood > 0
            && (e.WorkObjectSN == bait->SN || atkDist(e.DR, e.UR, bait->DR, bait->UR)
                <= 6 * BLOCKSIDELENGTH)) danger = true;
        for (const auto& b : info.enemy_buildings) if (b.Type == BUILDING_ARROWTOWER && b.Blood > 0
            && atkDist(atkBD(b.BlockDR), atkBD(b.BlockUR), bait->DR, bait->UR)
                <= (DIS_ARROWTOWER + 1) * BLOCKSIDELENGTH) danger = true;
        m_assaultBlood = bait->Blood;
        if (danger || f - m_assaultFrame > 500 || m_atkPhase == ATK_FIGHT) m_assaultStage = 2;
        if (m_assaultStage == 2) {
            if (atkDist(bait->DR, bait->UR, m_assaultPX, m_assaultPY) <= 1.5 * BLOCKSIDELENGTH
                || f - m_assaultFrame > 1000) {
                m_assaultSN = -1; m_assaultStage = 0;
            } else atkMove(self, info, *bait, m_assaultPX, m_assaultPY, orders, true);
        } else {
            Point p;
            // 主力有可见守军时保持阵地，由诱敌兵继续探入，不能反复只走到同一个六格路点。
            if (atkPathStep(int(bait->DR/BLOCKSIDELENGTH),int(bait->UR/BLOCKSIDELENGTH),4,4,p)
                &&atkDist(atkBD(p.x),atkBD(p.y),atkBD(m_atkFrontX),atkBD(m_atkFrontY))<=12*BLOCKSIDELENGTH)
                atkMove(self, info, *bait, atkBD(p.x), atkBD(p.y), orders);
            int nearby = 0;
            const bool groupReady = atkReady(info, *priest, nearby, m_assaultSN);
            if (groupReady && quiet && enemies==0 && !towerThreat && !critical && !pulling
                && f - m_assaultFrame >= 50 && f - m_atkAdvanceFrame >= 75 && atkAdvance(2)) {
                // 空旷区每次只前移两格，祭司和护卫重新到位后才进行下一步。
                // 保持诱敌兵在前方观察，不能等每次往返超时才带全军移动。
                m_atkAdvanceFrame = f; m_assaultPushed = true;
            }
        }
    }
    bool outsideTowerFire=true;
    for(const auto& b:info.enemy_buildings)if(b.Type==BUILDING_ARROWTOWER&&b.Blood>0
        &&atkDist(atkBD(b.BlockDR),atkBD(b.BlockUR),priest->DR,priest->UR)
            <=(DIS_ARROWTOWER+2)*BLOCKSIDELENGTH)outsideTowerFire=false;
    if (m_atkPhase == ATK_FIGHT && engagedEnemies>0 && !critical && !pulling && !overwhelming && (!towerThreat||outsideTowerFire) && ready
        && f - m_atkAdvanceFrame >= 100) {
        bool distantOnly = enemies > 0;
        double nearestGuard = 1e18;
        for (const auto& e : info.enemy_armies) if (e.Blood > 0
            && (atkDist(e.DR, e.UR, atkBD(m_atkFrontX), atkBD(m_atkFrontY)) <= 14 * BLOCKSIDELENGTH
                || atkDist(e.DR, e.UR, priest->DR, priest->UR) <= 12 * BLOCKSIDELENGTH)) {
            nearestGuard = std::min(nearestGuard, atkDist(e.DR, e.UR,
                atkBD(m_atkFrontX), atkBD(m_atkFrontY)));
            if (e.WorkObjectSN == priest->SN || atkDist(e.DR, e.UR,
                priest->DR, priest->UR) <= 9 * BLOCKSIDELENGTH) distantOnly = false;
        }
        // 守军停在射程外时整队短步接近，不能只针对投石车。
        if (distantOnly && nearestGuard > 4.5 * BLOCKSIDELENGTH && atkAdvance(1))
            m_atkAdvanceFrame = f;
    }
    std::set<int> emergencyEscorts;
    // 被锁定时附近兵多也需要实际救援；人数不能代替阻止追兵。
    if(critical && (attackers>0 || escorts<6)) {
        std::vector<const tagArmy*> cover;
        for(const auto& a:info.armies)if(atkFighter(a)&&a.Sort!=AT_STONE_THROWER
            &&armyTravelPossible(info,a))cover.push_back(&a);
        std::sort(cover.begin(),cover.end(),[&](const tagArmy* a,const tagArmy* b){
            return atkDist(a->DR,a->UR,priest->DR,priest->UR)<atkDist(b->DR,b->UR,priest->DR,priest->UR);
        });
        for(int i=0;i<std::min(6,(int)cover.size());++i)emergencyEscorts.insert(cover[i]->SN);
    }
    std::set<int> towerEscorts;
    if (m_atkPhase != ATK_RALLY && !enemies && towerThreat && army >= 8) {
        std::vector<const tagArmy*> nearestArmy;
        for (const auto& a : info.armies) if (atkFighter(a)) nearestArmy.push_back(&a);
        std::sort(nearestArmy.begin(),nearestArmy.end(),[&](const tagArmy* a,const tagArmy* b) {
            return atkDist(a->DR,a->UR,priest->DR,priest->UR) < atkDist(b->DR,b->UR,priest->DR,priest->UR);
        });
        for (const auto* a : nearestArmy) {
            bool holdingTower = false;
            for (const auto& b : info.enemy_buildings)
                if (b.Type == BUILDING_ARROWTOWER && b.Blood > 0 && b.Project == a->SN) holdingTower = true;
            if (!holdingTower) towerEscorts.insert(a->SN);
            if (towerEscorts.size() >= 6) break;
        }
        int cover = 0;
        for (const auto& a : info.armies) if (atkFighter(a)
            && atkDist(a.DR,a.UR,atkBD(m_atkFrontX),atkBD(m_atkFrontY)) <= 10 * BLOCKSIDELENGTH) ++cover;
        double towerDistance = 1e18;
        for (const auto& b : info.enemy_buildings) if (b.Type == BUILDING_ARROWTOWER && b.Blood > 0)
            towerDistance = std::min(towerDistance,atkDist(atkBD(b.BlockDR),atkBD(b.BlockUR),priest->DR,priest->UR));
        bool coveredStep=false;Point next;
        if(m_siegeSN>=0&&ready&&quiet&&m_atkTrail.size()>=2
            &&atkPathStep(m_atkFrontX,m_atkFrontY,2,5,next)){
            const Point nextRear=m_atkTrail[m_atkTrail.size()-2];
            coveredStep=atkOpen(nextRear.x,nextRear.y,true)&&atkTowerCoverAt(info,nextRear);
        }
        // 塔已有健康部队吸火时可以短步靠近；每步检查祭司下一后排位置。
        if (!critical && !pulling && cover >= 6 && (towerDistance > (DIS_ARROWTOWER + 3) * BLOCKSIDELENGTH||coveredStep)
            && atkDist(priest->DR,priest->UR,atkBD(rear.x),atkBD(rear.y)) <= 3 * BLOCKSIDELENGTH
            && f - m_atkAdvanceFrame >= 100
            && atkAdvance(2)) m_atkAdvanceFrame=f;
    }
    // 清塔也要留下六名护卫，不能全军围塔而把祭司留在十多格外。
    // 战斗/布阵：保护祭司的远程威胁优先，其他近处敌军次之，再清箭塔。
    std::vector<const tagArmy*> soldiers;
    for (const auto& a : info.armies) if (atkFighter(a)) soldiers.push_back(&a);
    std::sort(soldiers.begin(), soldiers.end(), [](const tagArmy* a, const tagArmy* b) {
        const int af=atkLastOrderFrame(a->SN),bf=atkLastOrderFrame(b->SN);
        return af==bf?a->SN<b->SN:af<bf;
    });
    std::set<int> towerPositions;
    std::map<int,int> assigned;
    for (const auto* a:soldiers) if(a->WorkObjectSN>0) ++assigned[a->WorkObjectSN];
    for (const tagArmy* a : soldiers) {
        if (orders >= ATK_MAX_ORDER || m_issued.count(a->SN)) continue;
        if(a->Sort==AT_STONE_THROWER&&a->WorkObjectSN>0&&!defenseStoneSafe(info,*a,a->WorkObjectSN)){
            self->HumanMove(a->SN,a->DR,a->UR);m_issued.insert(a->SN);++orders;continue;
        }
        if (a->SN == m_assaultSN && m_assaultStage != 0) continue;
        if(atkDodgeStone(self,info,*a,*priest,orders))continue;
        bool canFire=false;
        if(atkRanged(*a)&&a->Sort!=AT_STONE_THROWER)
            for(const auto& e:info.enemy_armies)if(e.Blood>0
                &&atkDist(a->DR,a->UR,e.DR,e.UR)<=atkEngageDistance(*a,pulling)*BLOCKSIDELENGTH
                &&atkDist(priest->DR,priest->UR,e.DR,e.UR)<=12*BLOCKSIDELENGTH)canFire=true;
        // 够得到的敌兵优先射击，不能被旧撤退槽位反复打断；贴身避险仍在下方处理。
        if(pulling && !emergencyEscorts.count(a->SN)&&!canFire){
            const auto slot=m_atkSlots.find(a->SN);
            if(slot!=m_atkSlots.end()
                &&atkDist(a->DR,a->UR,atkBD(slot->second.x),atkBD(slot->second.y))>3*BLOCKSIDELENGTH){
                atkMove(self,info,*a,atkBD(slot->second.x),atkBD(slot->second.y),orders,true);
                continue;
            }
        }
        if(emergencyEscorts.count(a->SN)) {
            int threat=-1;double bestThreat=-1e18;
            for(const auto& e:info.enemy_armies)if(e.Blood>0
                &&(e.WorkObjectSN==priest->SN||atkDist(e.DR,e.UR,priest->DR,priest->UR)<4*BLOCKSIDELENGTH)) {
                const double d=atkDist(a->DR,a->UR,e.DR,e.UR);
                if(d>(atkRanged(*a)?7.0:2.5)*BLOCKSIDELENGTH)continue;
                const double score=(e.WorkObjectSN==priest->SN?1000:0)-d;
                if(score>bestThreat){bestThreat=score;threat=e.SN;}
            }
            // 没有敌军贴身时，护卫可以原地射击够得到的箭塔；不能因祭司低血而全体停火。
            if(threat<0 && atkRanged(*a)
                &&atkDist(a->DR,a->UR,priest->DR,priest->UR)<=6*BLOCKSIDELENGTH){
                const double range=defenseFireRange(*a)-0.25;
                double closest=range*BLOCKSIDELENGTH;
                for(const auto& b:info.enemy_buildings)if(b.Type==BUILDING_ARROWTOWER&&b.Blood>0){
                    const double d=atkDist(a->DR,a->UR,atkBD(b.BlockDR),atkBD(b.BlockUR));
                    if(d<closest){closest=d;threat=b.SN;}
                }
            }
            if(threat>=0)atkAction(self,info,*a,threat,orders);
            else {
                Point cover,step;
                const int px=(int)(priest->DR/BLOCKSIDELENGTH),py=(int)(priest->UR/BLOCKSIDELENGTH);
                if(atkFindOpen(px+(a->SN%3-1)*2,py+(a->SN%2?2:-2),2,cover)) {
                    if(atkRouteStep((int)(a->DR/BLOCKSIDELENGTH),(int)(a->UR/BLOCKSIDELENGTH),
                        cover.x,cover.y,6,1,step))cover=step;
                    atkMove(self,info,*a,atkBD(cover.x),atkBD(cover.y),orders,a->NowState!=HUMAN_STATE_WALKING);
                }
            }
            continue;
        }
        if (towerEscorts.count(a->SN)) {
            // 建筑转换会自动走到建筑旁边；护卫必须跟随实际祭司，不能守旧槽位。
            if(convertingFactory&&atkDist(a->DR,a->UR,priest->DR,priest->UR)>6*BLOCKSIDELENGTH){
                Point position,step;
                const int px=(int)(priest->DR/BLOCKSIDELENGTH),py=(int)(priest->UR/BLOCKSIDELENGTH);
                if(atkFindOpen(px+(a->SN%3-1)*2,py+(a->SN%2?2:-2),2,position,true)){
                    if(atkRouteStep((int)(a->DR/BLOCKSIDELENGTH),(int)(a->UR/BLOCKSIDELENGTH),
                        position.x,position.y,5,1,step))position=step;
                    if(atkMove(self,info,*a,atkBD(position.x),atkBD(position.y),orders,true))continue;
                }
            }
            const double range=defenseFireRange(*a)-0.25;
            int tower = -1; double closest = range * BLOCKSIDELENGTH;
            if (atkDist(a->DR,a->UR,priest->DR,priest->UR) <= 9 * BLOCKSIDELENGTH)
                for (const auto& b : info.enemy_buildings) if (b.Type == BUILDING_ARROWTOWER && b.Blood > 0) {
                    const double d = atkDist(a->DR,a->UR,atkBD(b.BlockDR),atkBD(b.BlockUR));
                    if (d < closest) { closest=d; tower=b.SN; }
                }
            // 能射击就保留攻击；够不到时在保护圈内找可达射击位置。
            if (tower >= 0 && atkRanged(*a)) { atkAction(self,info,*a,tower,orders); continue; }
            if(atkRanged(*a)&&f-m_atkMoves[a->SN].orderFrame>=50){
                const tagBuilding* nearestTower=nullptr;Point firing,step;
                double distance=1e18;
                for(const auto& b:info.enemy_buildings)if(b.Type==BUILDING_ARROWTOWER&&b.Blood>0){
                    const double d=atkDist(a->DR,a->UR,atkBD(b.BlockDR),atkBD(b.BlockUR));
                    Point candidate,waypoint;
                    if(d<distance&&atkTowerPosition(*a,*priest,b,range,towerPositions,candidate,waypoint)){
                        nearestTower=&b;firing=candidate;step=waypoint;distance=d;
                    }
                }
                if(nearestTower){
                    towerPositions.insert(firing.x*100+firing.y);
                    if(atkMove(self,info,*a,atkBD(step.x),atkBD(step.y),orders,true))continue;
                }
            }
            // 正向保护圈内的射击点行走时，不能下一帧又被旧槽位叫回。
            const auto movement=m_atkMoves.find(a->SN);
            if(a->NowState==HUMAN_STATE_WALKING&&movement!=m_atkMoves.end()
                &&f-movement->second.orderFrame<50)continue;
            const auto slot = m_atkSlots.find(a->SN);
            if (slot != m_atkSlots.end()) atkMove(self,info,*a,atkBD(slot->second.x),atkBD(slot->second.y),orders,true);
            continue;
        }
        const auto dodge = m_atkDodgeFrames.find(a->SN);
        if(dodge!=m_atkDodgeFrames.end() && f-dodge->second<35 && a->NowState==HUMAN_STATE_WALKING) continue;
        if (atkRanged(*a) && a->Sort != AT_STONE_THROWER) {
            const tagArmy* close = nullptr;
            double nearestMelee = 3.5 * BLOCKSIDELENGTH;
            for (const auto& e : info.enemy_armies) {
                if (e.Blood <= 0 || atkRanged(e) || e.Sort == AT_PRIEST) continue;
                const double d = atkDist(a->DR, a->UR, e.DR, e.UR);
                if (d < nearestMelee) { nearestMelee = d; close = &e; }
            }
            if (close) {
                double dx = a->DR - close->DR, dy = a->UR - close->UR;
                double length = std::hypot(dx, dy);
                if (length < 0.1) { dx = priest->DR - close->DR; dy = priest->UR - close->UR; length = std::hypot(dx, dy); }
                Point retreat;
                if (length > 0.1 && atkFindOpen((int)(a->DR / BLOCKSIDELENGTH) + (int)std::lround(dx / length * 3),
                    (int)(a->UR / BLOCKSIDELENGTH) + (int)std::lround(dy / length * 3), 2, retreat)) {
                    atkMove(self, info, *a, atkBD(retreat.x), atkBD(retreat.y), orders, true);
                    continue;
                }
            }
        }
        // 建筑动作快照都是WORKING；卡在射程外的弓兵需先走近，不能永远保留同一攻击令。
        if (!enemies && atkRanged(*a) && a->Sort != AT_STONE_THROWER && a->WorkObjectSN > 0) {
            for (const auto& b : info.enemy_buildings) if (b.Type == BUILDING_ARROWTOWER
                && b.Blood > 0 && b.SN == a->WorkObjectSN
                && atkDist(a->DR,a->UR,atkBD(b.BlockDR),atkBD(b.BlockUR)) > 7 * BLOCKSIDELENGTH) {
                AtkMoveState& movement=m_atkMoves[a->SN];
                if (movement.progressFrame < 0 || atkDist(a->DR,a->UR,movement.px,movement.py) > BLOCKSIDELENGTH) {
                    movement.progressFrame=f; movement.px=a->DR; movement.py=a->UR;
                }
                Point step;
                if (f-movement.progressFrame>=200
                    && atkRouteStep((int)(a->DR/BLOCKSIDELENGTH),(int)(a->UR/BLOCKSIDELENGTH),
                        b.BlockDR,b.BlockUR,3,5,step)
                    && atkMove(self,info,*a,atkBD(step.x),atkBD(step.y),orders,true)) {
                    m_atkDodgeFrames[a->SN]=f; m_atkActions[a->SN]=f;
                }
                break;
            }
            if (m_issued.count(a->SN)) continue;
        }
        int target = -1; double best = -1e18;
        for (const auto& e : info.enemy_armies) if (e.Blood > 0) {
            const double d = atkDist(a->DR, a->UR, e.DR, e.UR);
            const double dp = atkDist(priest->DR, priest->UR, e.DR, e.UR);
            // 已形成的阵型只攻击自身射程或短追击范围，避免一次拉出整个敌营。
            const double engage=atkEngageDistance(*a,pulling);
            if (d > engage * BLOCKSIDELENGTH
                || (dp > 12 * BLOCKSIDELENGTH && atkDist(e.DR, e.UR,
                    atkBD(m_atkFrontX), atkBD(m_atkFrontY)) > 14 * BLOCKSIDELENGTH)) continue;
            const double value = (e.WorkObjectSN == priest->SN ? 1000 : 0)
                + (atkRanged(e) ? 200 : 0)
                + (!atkRanged(*a) && e.Sort == AT_STONE_THROWER ? 300 : 0)
                - assigned[e.SN]*10 - d / BLOCKSIDELENGTH;
            if (value > best) { best = value; target = e.SN; }
        }
        if (target < 0 && m_atkPhase != ATK_RALLY && !enemies)
            for (const auto& b : info.enemy_buildings) {
                if (b.Type != BUILDING_ARROWTOWER || b.Blood <= 0) continue;
                const double d = atkDist(a->DR, a->UR, atkBD(b.BlockDR), atkBD(b.BlockUR));
                if (d <= 16 * BLOCKSIDELENGTH && -d > best) { best = -d; target = b.SN; }
            }
        if (target >= 0) {
            if (atkAction(self, info, *a, target, orders)) {
                if(a->WorkObjectSN>0) --assigned[a->WorkObjectSN];
                ++assigned[target];
            }
        }
        else {
            bool chasing=false;
            for(const auto& e:info.enemy_armies)if(e.SN==a->WorkObjectSN && e.Blood>0)
                chasing=atkDist(a->DR,a->UR,e.DR,e.UR)>atkEngageDistance(*a,pulling)*BLOCKSIDELENGTH;
            if(enemies && a->NowState==HUMAN_STATE_WALKING && !chasing)continue;
            auto slot = m_atkSlots.find(a->SN);
            if (slot != m_atkSlots.end()) {
                const bool moved=atkMove(self, info, *a, atkBD(slot->second.x), atkBD(slot->second.y), orders,
                    chasing || a->NowState == HUMAN_STATE_ATTACKING || a->NowState == HUMAN_STATE_WORKING);
                if(!moved && chasing && orders<ATK_MAX_ORDER && !m_issued.count(a->SN)
                    &&f-m_atkMoves[a->SN].orderFrame>=25){
                    self->HumanMove(a->SN,a->DR,a->UR);m_issued.insert(a->SN);++orders;
                    m_atkMoves[a->SN].orderFrame=f;
                }
                // 已经在槽位但旧工作令仍指向塔时，也必须取消自动追塔。
                if(!moved&&enemies&&orders<ATK_MAX_ORDER&&!m_issued.count(a->SN))
                    for(const auto& b:info.enemy_buildings)if(b.SN==a->WorkObjectSN&&b.Type==BUILDING_ARROWTOWER){
                        self->HumanMove(a->SN,a->DR,a->UR);m_issued.insert(a->SN);++orders;break;
                    }
            }
        }
    }
    if (aiTraceEnabled() && f - m_atkDbgFrame >= 250) {
        m_atkDbgFrame = f;
        std::cout << "[ATK] f=" << f << " phase=" << m_atkPhase << " army=" << army
                  << " ready=" << ready << " escorts=" << escorts << " enemies=" << enemies
                  << " engaged=" << engagedEnemies
                  << " tower=" << towerThreat << " priestHP=" << priest->Blood
                  << " front=" << m_atkFrontX << "," << m_atkFrontY
                  << " target=" << m_atkTargetX << "," << m_atkTargetY
                  << " siege=" << m_siegeSN << " priestState=" << priest->NowState
                  << " priestWork=" << priest->WorkObjectSN
                  << " priestPos=" << priest->DR / BLOCKSIDELENGTH << "," << priest->UR / BLOCKSIDELENGTH
                  << " rear=" << rear.x << "," << rear.y
                  << " hold="<<m_atkHolding<<" factoryChance="<<factoryOpportunity
                  << " bait=" << m_assaultSN << ":" << m_assaultStage << std::endl;
        int enemyReported = 0;
        for (const auto& e : info.enemy_armies) if (e.Blood > 0 && enemyReported++ < 4)
            std::cout << "[ENEMY] sn=" << e.SN << " sort=" << e.Sort << " hp=" << e.Blood
                << " pos=" << e.DR / BLOCKSIDELENGTH << "," << e.UR / BLOCKSIDELENGTH
                << " work=" << e.WorkObjectSN << std::endl;
        for (const auto& b : info.enemy_buildings) if (b.Type == BUILDING_ARROWTOWER)
            std::cout << "[TOWER] sn=" << b.SN << " hp=" << b.Blood
                << " pos=" << b.BlockDR << "," << b.BlockUR << " target=" << b.Project << std::endl;
        int reported=0;
        for(const auto& a:info.armies) if(atkFighter(a)&&m_atkCoreSquad.count(a.SN)) {
            auto slot=m_atkSlots.find(a.SN);
            if(slot==m_atkSlots.end()) continue;
            if(atkDist(a.DR,a.UR,atkBD(slot->second.x),atkBD(slot->second.y))>2*BLOCKSIDELENGTH&&reported++<3)
                std::cout<<"[LAG] sn="<<a.SN<<" state="<<a.NowState
                    <<" work="<<a.WorkObjectSN
                    <<" pos="<<a.DR/BLOCKSIDELENGTH<<","<<a.UR/BLOCKSIDELENGTH
                    <<" slot="<<slot->second.x<<","<<slot->second.y<<std::endl;
        }
    }
}

void UsrAI::processData()
{
    tagInfo info = getInfo();       // 每帧获取游戏快照
    // 非帧同步下重复唤醒可能读取同一帧，不能向同一快照反复发令。
    static int lastProcessedFrame=-1;
    if(info.GameFrame<=lastProcessedFrame)return;
    lastProcessedFrame=info.GameFrame;
    maintainBuildRequests(info);
    econTrace(info);
    if(aiTraceEnabled()){
        static int previousHP=-1,previousFrame=-1,lastDamageReport=-9999;
        const tagArmy* priest=nullptr;for(const auto& a:info.armies)if(a.Sort==AT_PRIEST){priest=&a;break;}
        const int hp=priest?priest->Blood:-1;
        if(previousFrame>=0&&info.GameFrame>previousFrame&&previousHP>0
            &&(hp<0||(hp<previousHP&&info.GameFrame-lastDamageReport>=25))){
            lastDamageReport=info.GameFrame;
            std::cout<<"[PRIEST_HP] f="<<info.GameFrame<<" hp="<<previousHP<<"->"<<hp<<" attack="<<m_atkOn;
            if(priest){
                std::cout<<" pos="<<priest->DR/BLOCKSIDELENGTH<<","<<priest->UR/BLOCKSIDELENGTH
                    <<" work="<<priest->WorkObjectSN<<" nearby=";
                int reported=0;for(const auto& e:info.enemy_armies)if(e.Blood>0&&reported<8
                    &&calDistance(e.DR,e.UR,priest->DR,priest->UR)<=14*BLOCKSIDELENGTH){
                    ++reported;std::cout<<e.SN<<":"<<e.Sort<<":"<<e.WorkObjectSN<<":"
                        <<calDistance(e.DR,e.UR,priest->DR,priest->UR)/BLOCKSIDELENGTH<<",";
                }
                for(const auto& b:info.enemy_buildings)if(b.Type==BUILDING_ARROWTOWER&&b.Blood>0
                    &&calDistance((b.BlockDR+0.5)*BLOCKSIDELENGTH,(b.BlockUR+0.5)*BLOCKSIDELENGTH,priest->DR,priest->UR)<12*BLOCKSIDELENGTH)
                    std::cout<<" tower="<<b.SN<<":"<<b.Project<<":"<<b.BlockDR<<","<<b.BlockUR;
                for(const auto& a:info.armies)if(a.Blood>0&&a.Sort==AT_STONE_THROWER){
                    std::cout<<" ownStone="<<a.SN<<":"<<a.WorkObjectSN<<":"
                        <<a.DR/BLOCKSIDELENGTH<<","<<a.UR/BLOCKSIDELENGTH;
                }
                for(const auto& b:info.buildings)if(b.Type==BUILDING_ARROWTOWER&&b.Blood>0
                    &&calDistance((b.BlockDR+0.5)*BLOCKSIDELENGTH,(b.BlockUR+0.5)*BLOCKSIDELENGTH,priest->DR,priest->UR)<12*BLOCKSIDELENGTH)
                    std::cout<<" ownTower="<<b.SN<<":"<<b.Project<<":"<<b.BlockDR<<","<<b.BlockUR;
            }
            std::cout<<std::endl;
        }
        previousHP=hp;previousFrame=info.GameFrame;
    }
    static int waveReportFrame = -9999;
    if (aiTraceEnabled() && info.GameFrame >= 13000 && info.GameFrame - waveReportFrame >= 250) {
        waveReportFrame = info.GameFrame;
        int priestHP = -1, units = 0, bows = 0, comps = 0, melee = 0, idle = 0;
        int woodWorkers = 0, goldWorkers = 0, foodWorkers = 0;
        const ArmyMix mix=collectArmyMix(info);
        for (const auto& a : info.armies) {
            if (a.Sort == AT_PRIEST) priestHP = a.Blood;
            else if (a.Blood > 0) {
                ++units;
                if (a.Sort == AT_BOWMAN) ++bows;
                else if (a.Sort == AT_COMPOSITE_BOWMAN) ++comps;
                else ++melee;
            }
        }
        for (const auto& v : info.farmers) {
            if (v.NowState == HUMAN_STATE_IDLE) ++idle;
            const int role = m_role[v.SN];
            if (role == 2) ++woodWorkers;
            else if (role == 3) ++goldWorkers;
            else if (role == 1 || role == 4 || role == 5) ++foodWorkers;
        }
        std::cout << "[W3] f=" << info.GameFrame << " age=" << info.civilizationStage
                  << " priest=" << priestHP << " units=" << units << " bow=" << bows
                  << " comp=" << comps << " melee=" << melee
                  << " front=" << mix.frontline << " frontHealthy=" << mix.healthyFrontline
                  << " queuedComp=" << mix.queuedComposite << " queuedFront=" << mix.queuedFrontline
                  << " farmers=" << info.farmers.size()
                  << " pop=" << info.Human_Num << "/" << info.Human_MaxNum
                  << " builder=" << m_builderSN
                  << " idle=" << idle << " jobs=" << foodWorkers << "/" << woodWorkers << "/" << goldWorkers
                  << " food=" << info.Meat << " wood=" << info.Wood << " gold=" << info.Gold
                  << " compTech=" << m_researchCount[BUILDING_RANGE_UPGRADE_COMPOSITE_BOW]
                  << " enemy=" << info.enemy_armies.size();
        for (const auto& b : info.buildings) {
            if (b.Type == BUILDING_ARROWTOWER) std::cout << " tower" << b.SN << "=" << b.Blood;
            if (b.Type == BUILDING_RANGE) std::cout << " range=" << b.Project << ":" << b.ProjectPercent;
            if (b.Type == BUILDING_ARMYCAMP || b.Type == BUILDING_STABLE || b.Type == BUILDING_COLLAGE)
                std::cout << " producer=" << b.SN << ":" << b.Type << ":" << b.Percent
                    << ":" << b.Project << ":" << b.ProjectPercent;
            if (b.Type == BUILDING_CENTER) std::cout << " centerHP=" << b.Blood;
        }
        std::cout << std::endl;
        if (!m_atkOn && !info.enemy_armies.empty()) {
            for (const auto& a : info.armies) if (a.Sort == AT_PRIEST)
                std::cout << "[DEF] priest=" << a.SN << " state=" << a.NowState << " work=" << a.WorkObjectSN
                    << " cooldown=" << a.ConvertCooldown << " pos=" << a.DR/BLOCKSIDELENGTH << "," << a.UR/BLOCKSIDELENGTH << std::endl;
            for (const auto& e : info.enemy_armies)
                std::cout << "[DEFENEMY] sn=" << e.SN << " sort=" << e.Sort << " hp=" << e.Blood
                    << " work=" << e.WorkObjectSN << " pos=" << e.DR/BLOCKSIDELENGTH << "," << e.UR/BLOCKSIDELENGTH << std::endl;
        }
    }
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
    // 死亡当帧接替，不等待下一帧；仍只保留一位专职建造者。
    bool builderAlive=false;
    for(const auto& f:info.farmers)if(f.SN==m_builderSN&&f.Blood>0)builderAlive=true;
    if(!builderAlive)m_builderSN=-1;
    if (m_builderSN < 0) {
        for (const tagFarmer& f : info.farmers) {
            if (f.FarmerSort == FARMERTYPE_FARMER&&f.Blood>0&&f.SN!=m_depotBuilderSN) { m_builderSN = f.SN; break; }
        }
    }

    confirmResearch(info);
    updateMap(info);                // 建立地图 + 收集已探明空地
    farmerDamageTrace(info);        // 只记录掉血/失踪事件，不下令，不改采集与生产策略。
    recoverIdleCargo(this,info);    // v65：空闲载货者先交付，再恢复生产。
    preserveFarmerCargo(this,info);  // 携带资源且任务有效时，避免改派打断正常采集/返仓。
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
