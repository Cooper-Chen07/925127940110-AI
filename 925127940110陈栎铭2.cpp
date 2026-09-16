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
    if (m_issued.count(priestSN)) return;           // 本帧已被其他模块下令（如避险撤退）

    // 1.5) 【修复·反复移动】场上有敌人（第一波开打/波次残兵）时祭司不探路：
    //      专心贴塔 + 转化，否则"探路目标"会和 handlePriest 的"回塔待命"互相打断。
    if (!info.enemy_armies.empty() || !info.enemy_farmers.empty()) {
        if (m_centerX >= 0) {
            int hx, hy;
            getPriestHome(info, hx, hy);
            double homeDR = (double)hx * BLOCKSIDELENGTH;
            double homeUR = (double)hy * BLOCKSIDELENGTH;
            if (calDistance(priest->DR, priest->UR, homeDR, homeUR) > 2.0 * BLOCKSIDELENGTH
                && priest->NowState == HUMAN_STATE_IDLE) {
                movePriest(priestSN, priest->DR, priest->UR, homeDR, homeUR, info.GameFrame);
            }
        }
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
void UsrAI::manageVillagers(const tagInfo& info)
{
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

    // ===== 【用户要求·铜器后人员配额】=====
    //   采金 5 人（固定）；木材 4 人，木材不够(<150)时临时加到 6 人；
    //   其余**全部采集食物**——食物是造兵/科技的唯一瓶颈（实测食物只有 10~28 时
    //   180 食的复合弓科技和造兵全卡住，而黄金却堆到 260）。
    if (bronze) {
        targetGold = 5;
        targetWood = (info.Wood < 150) ? 6 : 4;    // 木材 4 人；不够时临时加到 6
        targetFood = total - targetGold - targetWood;   // 其余全采食物
        if (targetFood < 5) targetFood = 5;
    }

    // 3) 逐个给空闲农民分配工作
    //    额外处理：非空闲但"工作目标失效"的农民（如猎取的羚羊尸体已被采完）
    //    也重新分配，避免卡在无效目标上不动
    for (const tagFarmer& f : info.farmers) {
        if (f.FarmerSort != FARMERTYPE_FARMER) continue;
        if (f.SN == m_builderSN) continue;              // 专职建造者不参与采集（由 buildBuildings 调度）
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
                    if (wantR < 3) wantR = 3;
                    if (wantR > 8) wantR = 8;
                    if (countBuilding(info, BUILDING_FARM) < wantR
                        && info.Wood >= BUILD_FARM_WOOD) {
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
                    if (timeout > 0) {
                        suspect = true;
                        auto it = m_moveStart.find(f.SN);
                        auto id = m_lastDist.find(f.SN);
                        if (it == m_moveStart.end() || id == m_lastDist.end()) {
                            m_moveStart[f.SN] = info.GameFrame;
                            m_lastDist[f.SN] = tDist;
                        } else if (tDist < id->second - 0.2 * BLOCKSIDELENGTH) {
                            m_moveStart[f.SN] = info.GameFrame;   // 在靠近 → 有进展，重置计时
                            m_lastDist[f.SN] = tDist;
                        } else {
                            m_lastDist[f.SN] = tDist;
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
            if (m_huntWaiting && !wave2Farm) continue;
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
                    if (users >= 1) continue;                       // 已经有 1 个农民 → 不再派人
                    double d = calDistance(f.DR, f.UR,
                                           (double)fb.BlockDR * BLOCKSIDELENGTH,
                                           (double)fb.BlockUR * BLOCKSIDELENGTH);
                    if (d < bestFarmD) { bestFarmD = d; farmSN = fb.SN; }
                }
                if (farmSN >= 0) {
                    HumanAction(f.SN, farmSN);
                    m_issued.insert(f.SN);
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
                    if (farmWantV < 3) farmWantV = 3;
                    if (farmWantV > 8) farmWantV = 8;
                    if (countBuilding(info, BUILDING_FARM) < farmWantV
                        && (!mapFoodLeft(info) || info.Meat < 200)
                        && info.Wood >= BUILD_FARM_WOOD) {
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
            if (c >= 4) continue;
            if (c == 0 && idleFarmers < 3) continue;
            if (c < eleBestCnt) { eleBestCnt = c; eleBestSn = sn; }
        }
        if (anyElephant && eleBestSn >= 0) return eleBestSn;
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
    std::vector<const tagResource*> trees;
    for (const tagResource& r : info.resources)
        if (r.Type == RESOURCE_TREE && r.Cnt > 0) trees.push_back(&r);
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
        //   现在：人口接近上限就继续补房（最多 10 座 = 44 人口）。
        //   注意只在靶场已建后才补，避免抢在"市场/兵营/靶场"这条升级关键链之前。
        int homes = countBuilding(info, BUILDING_HOME);
        int houseTarget = (countBuilding(info, BUILDING_RANGE) > 0) ? 6 : 4;
        bool bronzeNow2 = (info.civilizationStage >= CIVILIZATION_BRONZEAGE);
        if (bronzeNow2 && countBuilding(info, BUILDING_RANGE) > 0) houseTarget = 8;
        if (countBuilding(info, BUILDING_RANGE) > 0
            && (int)info.Human_Num >= (int)info.Human_MaxNum - 2
            && homes < 10) {
            houseTarget = homes + 1;         // 人口卡住 → 再加一座房（给军队腾人口）
        }
        // 【用户要求·第二波后·一人一田】农田目标改为按"食物采集人数"（= 农民的一半）扩田：
        //   打猎采光后要转种田的农民，必须先有田可种 → 先把农田建出来（每块 75 木）
        //   上限 8 块（木头不够时自然停下）；第二波前仍保持 3 块
        // 【用户要求】地图上还有浆果/动物 → 优先采它们：农田最多保留 3 块（用户原策略），
        //   等地图食物采光后才按"食物采集人数"扩建（最多 8 块）→ 不提前浪费木头
        int farmWant = 3;
        // 【修复】食物告急（<200）时不被"地图还有食物"挡住：远水不解近渴，先把田开出来
        if (info.GameFrame > FRAME_WAVE2 && (!mapFoodLeft(info) || info.Meat < 200)) {
            farmWant = (int)info.farmers.size() / 2;
            if (farmWant < 3) farmWant = 3;
            if (farmWant > 8) farmWant = 8;
        }

        // 记录箭塔位置（靶场要建在箭塔附近）
        int towerBX = -1, towerBY = -1;
        for (const tagBuilding& tb : info.buildings)
            if (tb.Type == BUILDING_ARROWTOWER) { towerBX = tb.BlockDR; towerBY = tb.BlockUR; break; }

        // ===== 【紧急·食物告急】第二波后食物见底 → 专职建造者先把农田补出来 =====
        //   农田采空会被引擎直接删除（Core.cpp:255-269 "采集完成"）→ 必须不断补种；
        //   而建造链里农田排在最后，第二波后 马厩/学院/箭塔/住房 会把建造者占满 → 农田轮不到。
        if (!built && info.GameFrame > FRAME_WAVE2 && info.Meat < 150
            && countBuilding(info, BUILDING_FARM) < farmWant
            && countBuilding(info, BUILDING_MARKET) > 0
            && info.Wood >= BUILD_FARM_WOOD) {
            int gxe = m_centerX, gye = m_centerY;
            for (const tagBuilding& b : info.buildings)
                if (b.Type == BUILDING_GRANARY) { gxe = b.BlockDR; gye = b.BlockUR; break; }
            int xe, ye;
            if (findBuildBlock(info, xe, ye, 3, 3, gxe, gye)) {
                HumanBuild(builder, BUILDING_FARM, xe, ye);
                m_issued.insert(builder);
                return;                     // 本帧只下这一条令
            }
        }

        // ===== 【紧急·人口满】人口卡住 → 住房抢先 =====
        //   实测：房5、人口22/24、调试行一直显示"造兵:人口已满(需补房)"，但唯一建造者被
        //   农田告急/马厩/学院/箭塔 依次占着 → 住房排在最后永远轮不到 → 兵和农民都造不出来。
        //   住房只要 30 木却能立刻解锁人口，所以放在"食物紧急"之后、其它建设之前。
        if (!built && countBuilding(info, BUILDING_HOME) < houseTarget
            && (int)info.Human_Num >= (int)info.Human_MaxNum - 2
            && info.Wood >= BUILD_HOUSE_WOOD) {
            int xh, yh;
            if (findBuildBlock(info, xh, yh, 2, 2)) {
                HumanBuild(builder, BUILDING_HOME, xh, yh);
                m_issued.insert(builder);
                return;
            }
        }

        // ===== 【3.0.7g 新增·第二波后发育阶段】帧 > FRAME_WAVE2 时的建设优先级 =====
        //   为什么需要：马厩/学院原本排在 else-if 链末尾（住房→市场→兵营→靶场→马厩→学院），
        //   而"人口临界就补房"（houseTarget = 现有房数+1）会让**住房分支永远成立**，
        //   把马厩/学院彻底挡在后面 → 第二波之后骑兵/方阵兵永远出不来。
        //   第二波打完（经济已成型）后把这两栋提到链首，为第三波（2 投石车 + 战车弓/复合弓）
        //   和反攻攒兵：骑兵(速度4/150血，切投石车、救祭司) + 方阵兵(120血/17攻，正面肉盾)。
        bool afterWave2 = (info.GameFrame > FRAME_WAVE2);
        if (afterWave2) {
            if (countBuilding(info, BUILDING_STABLE) == 0 && info.Wood >= BUILD_STABLE_WOOD) {
                int sx, sy;
                if (findBuildBlock(info, sx, sy, 3, 3)) {
                    HumanBuild(builder, BUILDING_STABLE, sx, sy);
                    m_issued.insert(builder);
                    return;                     // 本帧只下这一条令（保证一帧只有一条建造指令）
                }
            }
            if (countBuilding(info, BUILDING_COLLAGE) == 0
                && info.Wood >= BUILD_COLLAGE_WOOD) {
                int cx2, cy2;
                if (findBuildBlock(info, cx2, cy2, 3, 3)) {
                    HumanBuild(builder, BUILDING_COLLAGE, cx2, cy2);
                    m_issued.insert(builder);
                    return;
                }
            }
        }

        // ===== 【用户要求】靶场建好后立刻补第二座箭塔 =====
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

        // 1) 住房（先建到 4 座）
        if (countBuilding(info, BUILDING_HOME) < houseTarget && info.Wood >= BUILD_HOUSE_WOOD) {
            int x, y;
            if (findBuildBlock(info, x, y, 2, 2)) {
                HumanBuild(builder, BUILDING_HOME, x, y);
                m_issued.insert(builder);
                built = true;
            }
        }
        // 2) 箭塔（1 座，用初始 150 石；发育策略：靶场建在它附近）
        else if (countBuilding(info, BUILDING_HOME) >= 4
            && m_researchCount[BUILDING_GRANARY_ARROWTOWER] > 0
            && countBuilding(info, BUILDING_ARROWTOWER) < 1
            && info.Stone >= BUILD_ARROWTOWER_STONE) {
            int x, y;
            if (findBuildBlock(info, x, y, 2, 2)) {
                HumanBuild(builder, BUILDING_ARROWTOWER, x, y);
                m_issued.insert(builder);
                built = true;
            }
        }
        // 3) 市场（【发育策略】先建市场 → 立即研发伐木科技，加速攒木头）
        else if (countBuilding(info, BUILDING_MARKET) == 0 && info.Wood >= BUILD_MARKET_WOOD) {
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
        // 6) 马厩（升级后首选：骑兵克远程）
        else if (info.civilizationStage >= CIVILIZATION_BRONZEAGE
            && countBuilding(info, BUILDING_STABLE) == 0 && info.Wood >= BUILD_STABLE_WOOD) {
            int x, y;
            if (findBuildBlock(info, x, y, 3, 3)) {
                HumanBuild(builder, BUILDING_STABLE, x, y);
                m_issued.insert(builder);
                built = true;
            }
        }
        // 7) 学院（升级后）
        else if (info.civilizationStage >= CIVILIZATION_BRONZEAGE
            && countBuilding(info, BUILDING_COLLAGE) == 0 && info.Wood >= BUILD_COLLAGE_WOOD) {
            int x, y;
            if (findBuildBlock(info, x, y, 3, 3)) {
                HumanBuild(builder, BUILDING_COLLAGE, x, y);
                m_issued.insert(builder);
                built = true;
            }
        }
        // 8) 农田（升级后，谷仓旁）
        else if (info.civilizationStage >= CIVILIZATION_BRONZEAGE
            && countBuilding(info, BUILDING_MARKET) > 0
            && countBuilding(info, BUILDING_FARM) < farmWant
            && info.Wood >= BUILD_FARM_WOOD) {
            int gx = m_centerX, gy = m_centerY;
            for (const tagBuilding& b : info.buildings)
                if (b.Type == BUILDING_GRANARY) { gx = b.BlockDR; gy = b.BlockUR; break; }
            int x, y;
            if (findBuildBlock(info, x, y, 3, 3, gx, gy)) {
                HumanBuild(builder, BUILDING_FARM, x, y);
                m_issued.insert(builder);
                built = true;
            }
        }
        // 6) 【发育策略】金矿旁仓库：靶场已建、两座新房已补（房≥6）、铜器后 → 建设者去金矿旁建仓库
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
                        built = true;
                    }
                }
            }
        }
        // （羚羊堆仓库/浆果堆谷仓由采集者负责，见 buildResourceDepots）
        if (!built) return;  // 无可建建筑 → 本帧结束（绝不抽调其他农民帮忙）
    }
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
            if (b.Percent < 100) continue;
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
            if (b.Percent < 100) continue;
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
                      && countBuilding(info, BUILDING_STOCK) < 3
                      && info.Wood >= BUILD_STOCK_WOOD);
    if (m_preyStockDone) {
        bool anyStock = false;
        for (const tagBuilding& b : info.buildings)
            if (b.Type == BUILDING_STOCK) { anyStock = true; break; }
        if (!anyStock && info.GameFrame - m_preyStockFrame > 600) m_preyStockDone = false;
    }
    bool needGranary = (farBush != nullptr && farBushD > NEED_DIST
                        && countBuilding(info, BUILDING_GRANARY) < 3
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
// 防守：箭塔"拉仇恨"——优先攻击满血（未标记）的敌人
// 规则：
//   ① 射程内优先选"满血"敌人（标记它/拉到仇恨，广覆盖每个进射程的敌人）
//   ② 满血敌人选"威胁祭司的"优先，其次近塔的
//   ③ 范围内没有满血了 → 才打已掉血的
//   ④ 切换节流：塔切换目标后 30 帧内不切换（防频繁切换导致塔永远不射击）
// ============================================================
void UsrAI::defense(const tagInfo& info)
{
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
    double homeDR = (double)hx * BLOCKSIDELENGTH;
    double homeUR = (double)hy * BLOCKSIDELENGTH;
    bool enemyVisible = !(info.enemy_armies.empty() && info.enemy_farmers.empty());

    for (const tagArmy& a : info.armies) {
        if (a.Sort == AT_PRIEST || a.Sort == AT_SCOUT) continue;   // 祭司/侦察骑兵单独调度
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
            // ① 战车弓兵绝对优先（第二波专杀祭司）：只要视野内有战车弓兵，所有兵优先锁定它
            //    拉仇恨：战车弓兵被攻击后会反击攻击者，从而保护祭司
            int target = -1;
            double bestR = 1e18;
            for (const tagArmy& e : info.enemy_armies) {
                if (e.Sort != AT_CHARIOT_ARCHER) continue;
                double d = calDistance(a.DR, a.UR, e.DR, e.UR);
                if (d < bestR) { bestR = d; target = e.SN; }
            }
            // ② 无战车弓兵 → 其他远程威胁（投石车>复合弓兵>弓箭手——打建筑/远程压制）
            if (target < 0) {
                bestR = 1e18;
                for (const tagArmy& e : info.enemy_armies) {
                    if (e.Sort != AT_STONE_THROWER
                        && e.Sort != AT_COMPOSITE_BOWMAN && e.Sort != AT_BOWMAN) continue;
                    double d = calDistance(a.DR, a.UR, e.DR, e.UR);
                    if (d < bestR) { bestR = d; target = e.SN; }
                }
            }
            // ③ 没有远程 → 攻击最近敌人
            if (target < 0) {
                double best = 1e18;
                for (const tagArmy& e : info.enemy_armies) {
                    double d = calDistance(a.DR, a.UR, e.DR, e.UR);
                    if (d < best) { best = d; target = e.SN; }
                }
                if (target < 0) {
                    for (const tagFarmer& e : info.enemy_farmers) {
                        double d = calDistance(a.DR, a.UR, e.DR, e.UR);
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
            if (calDistance(a.DR, a.UR, homeDR, homeUR) > 5.0 * BLOCKSIDELENGTH) {
                HumanMove(a.SN, homeDR, homeUR);
                m_issued.insert(a.SN);
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
    // 1) 找到祭司
    int priestSN = -1;
    const tagArmy* priest = nullptr;
    for (const tagArmy& a : info.armies) {
        if (a.Sort == AT_PRIEST) { priestSN = a.SN; priest = &a; break; }
    }
    if (priest == nullptr) return;                  // 祭司不存在（死亡=游戏失败）

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
    }
    // 快照延迟补偿：刚下令转化（150帧内）主线程快照可能还没把 WorkObjectSN 传回来
    bool justOrderedConvert = (m_convertStartFrame >= 0
                               && info.GameFrame - m_convertStartFrame < 150);
    bool inConversion = convertingNow || justOrderedConvert;
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
                    if (d < bestD) { bestD = d; target = e.SN; }
                }
            }
        }
        // ③ attackedByUs 为空 → target 保持 -1：本帧不转化（等兵先接战建立仇恨）
    }

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
    if (needConvertOrder) {
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
        const bool wave2Lure = (info.GameFrame > FRAME_WAVE2 - 3000
                                && info.GameFrame <= FRAME_WAVE3);
        if (wave2Lure && !inConversion && !criticalBlood) {
            const tagArmy* ca = nullptr;
            double caD = 1e18;
            for (const tagArmy& e : info.enemy_armies) {
                if (e.Blood <= 0 || e.Sort != AT_CHARIOT_ARCHER) continue;
                double d = calDistance(priest->DR, priest->UR, e.DR, e.UR);
                if (d < caD) { caD = d; ca = &e; }
            }
            if (ca != nullptr
                && (caD <= 14.0 * BLOCKSIDELENGTH || ca->WorkObjectSN == priestSN)) {
                int hx, hy;
                getPriestHome(info, hx, hy);
                if (hx >= 0) {
                    double gx = (double)hx * BLOCKSIDELENGTH;
                    double gy = (double)hy * BLOCKSIDELENGTH;
                    if (movePriest(priestSN, priest->DR, priest->UR, gx, gy, info.GameFrame))
                        return;             // 已下令撤向箭塔 → 本帧结束
                }
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
    if (!inConversion && threat != nullptr && nearest < threatDist) {
        int hx, hy;
        getPriestHome(info, hx, hy);
        if (hx >= 0) {
            movePriest(priestSN, priest->DR, priest->UR, (double)hx * BLOCKSIDELENGTH, (double)hy * BLOCKSIDELENGTH, info.GameFrame);
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

void UsrAI::processData()
{
    tagInfo info = getInfo();       // 每帧获取游戏快照
    m_issued.clear();               // 清空本帧已下令记录

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
    defense(info);                  // 箭塔"拉仇恨"：优先攻击满血敌人
    handlePriest(info);             // 祭司：贴塔拉怪/转化（优先于探路）
    scoutWithPriest(info);          // 祭司随机探路（若祭司本帧已避险则不执行）
    scoutWithScout(info);           // 侦察骑兵探路（无战事时，持续到第三波前）

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
#endif   // USRAI_DEBUG_LINE
}
