#include "UsrAI.h"
#include<set>
#include <iostream>
#include<unordered_map>
#include<list>
#include <cstdlib>

using namespace std;
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

    // 2) 探路结束 → 回祭司站位（双塔中点 > 单塔 > 市中心）
    //    提前结束：第一波前 1500 帧（4500帧）就回塔，留时间准备防御
    if (m_scoutIdx >= SCOUT_MAX_COUNT || info.GameFrame > FRAME_WAVE1 - 1500) {
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
// nearX/nearY >= 0 时从该位置附近开始搜索（用于指定建筑布局）
// ============================================================
bool UsrAI::findBuildBlock(const tagInfo& info, int& x, int& y, int w, int h, int nearX, int nearY)
{
    if (info.theMap == nullptr) return false;
    const auto& terrain = *info.theMap;

    // 起点：指定附近位置 > 缓存的搜索位置 > 市镇中心附近
    int startX = m_searchX, startY = m_searchY;
    if (nearX >= 0) {
        startX = (nearX > 3 ? nearX - 3 : 0);
        startY = (nearY > 3 ? nearY - 3 : 0);
    } else if (m_centerX > 0 && startX == 0 && startY == 0) {
        startX = (m_centerX > 8 ? m_centerX - 8 : 0);
        startY = (m_centerY > 8 ? m_centerY - 8 : 0);
    }

    // 两遍扫描：第一遍从起点开始，第二遍从头开始（覆盖起点之前的区域）
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = (pass == 0 ? startX : 0); i < 100; ++i) {
            for (int j = (pass == 0 && i == startX ? startY : 0); j < 100; ++j) {
                if (i + w > 100 || j + h > 100) continue;
                bool ok = true;
                int height = terrain[i][j].height;
                for (int di = 0; di < w && ok; ++di) {
                    for (int dj = 0; dj < h; ++dj) {
                        if (m_map[i + di][j + dj] != 0) { ok = false; break; }
                        if (terrain[i + di][j + dj].height != height) { ok = false; break; }
                    }
                }
                // 外扩 1 圈避开浆果丛：建筑不能紧挨采集点，否则农民采浆果会被卡住
                if (ok) {
                    for (int di = -1; di <= w && ok; ++di) {
                        for (int dj = -1; dj <= h; ++dj) {
                            if (di >= 0 && di < w && dj >= 0 && dj < h) continue;   // 跳过建筑内部
                            int nx = i + di, ny = j + dj;
                            if (nx < 0 || nx >= 100 || ny < 0 || ny >= 100) continue;
                            if (m_map[nx][ny] == 10 + RESOURCE_BUSH) { ok = false; break; }   // 紧挨浆果丛
                        }
                    }
                }
                if (ok) {
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
        if (f.NowState != HUMAN_STATE_WORKING) continue;
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
    std::unordered_map<int,int> targetCount;
    for (const tagFarmer& f : info.farmers)
        if (f.NowState != HUMAN_STATE_WORKING) targetCount[f.WorkObjectSN]++;

    // 3) 动态配额
    bool bronze = (info.civilizationStage >= CIVILIZATION_BRONZEAGE);
    int targetFood = total / 2;                 // 采粮人数 >= 总人数一半
    if (targetFood < 5) targetFood = 5;         // 保底 5 个
    int targetWood = 2;                         // 正常 2 个木工
    // 根据升级建筑进度判断：一座都还没建 → 急需木头，不足时加人；已建 1-2 座 → 不再增派人手
    int upgradeBuilt = 0;
    if (countBuilding(info, BUILDING_MARKET) > 0) upgradeBuilt++;
    if (countBuilding(info, BUILDING_RANGE) > 0) upgradeBuilt++;
    if (countBuilding(info, BUILDING_STABLE) > 0) upgradeBuilt++;
    if (upgradeBuilt == 0) {
        if (info.Wood < 150) targetWood = 3;    // 攒市场/兵营木头，加 1 人
        if (info.Wood < 60) targetWood = 4;     // 严重不足加 2 人
    }
    int targetStone = 1;
    int targetGold = bronze ? 3 : 0;            // 铜器后挖金，为造兵准备

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
            bool valid = false;
            for (const tagResource& r : info.resources)
                if (r.SN == f.WorkObjectSN && r.Cnt > 0) { valid = true; break; }
            if (!valid) {
                for (const tagBuilding& b : info.buildings)
                    if (b.SN == f.WorkObjectSN) { valid = true; break; }   // 农田等建筑目标
            }
            if (valid) {
                // 卡住检测：动物 60 帧；静态资源（浆果/树/石）120 帧（防止被建筑挡住罚站）
                // 只对近距离目标检测（远处目标走路正常，避免误伤）
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
                if (f.NowState == HUMAN_STATE_WALKING && hasTarget
                    && calDistance(f.DR, f.UR, targetDR, targetUR) < 15.0 * BLOCKSIDELENGTH) {
                    int timeout = isAnimal ? 60 : 120;
                    auto it = m_moveStart.find(f.SN);
                    if (it == m_moveStart.end()) {
                        m_moveStart[f.SN] = info.GameFrame;   // 记录开始移动帧
                        continue;
                    } else if (info.GameFrame - it->second <= timeout) {
                        continue;                              // 正常移动中
                    }
                    m_moveStart.erase(f.SN);                   // 超时 → 判定卡住，重新分配
                } else {
                    m_moveStart.erase(f.SN);                   // 远处目标/其他状态：正常工作不打扰
                    continue;
                }
            }
            // 目标失效或卡住 → 掉下去重新分配（新指令覆盖旧目标）
        }

        // ① 浆果：开局 4 人 + 第一波前新增 4 人（共 8 人），选"采集人数最少"的丛分散采
        //    采浆果的农民标记为专属食物采集者（浆果采完自动找下一个食物资源）
        if (berryExists && berryCnt < 8) {
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
                m_foodGatherers.insert(f.SN);   // 标记专属食物采集
                berryCnt++;
                foodCnt++;
                continue;
            }
        }
        // ② 木头（正常2人，按需动态）；专属食物采集者不砍树（只做食物）
        if (!isFood && woodCnt < targetWood) {
            int sn = findNearestResource(info, RESOURCE_TREE, f.SN);
            if (sn >= 0) {
                HumanAction(f.SN, sn);
                m_issued.insert(f.SN);
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
                stoneCnt++;
                continue;
            }
        }
        // ④ 打猎 → 种田 → 建农田
        //    专属食物采集者无条件做食物（浆果采完/猎物打完自动流转找下一个食物）；
        //    未升级时：所有空闲农民都优先采食物（尽快采完地图食物，不跑去砍树）；
        //    升级后：普通农民只在食物缺口时补位打猎（打猎也标记为专属，两两一组分散猎杀）
        if (isFood || !bronze || foodCnt < targetFood) {
            int sn = findNearestHunt(info, f.SN);
            if (sn >= 0) {
                HumanAction(f.SN, sn);
                m_issued.insert(f.SN);
                m_foodGatherers.insert(f.SN);   // 打猎也标记专属
                foodCnt++;
                huntCnt++;
                continue;
            }
            // 升级前不种田、不建农田（浆果/猎物撑到升级即可）
            if (info.civilizationStage >= CIVILIZATION_BRONZEAGE) {
                // 种田（已有农田）
                int farmSN = findNearestFarm(info, f.SN);
                if (farmSN >= 0) {
                    HumanAction(f.SN, farmSN);
                    m_issued.insert(f.SN);
                    m_foodGatherers.insert(f.SN);
                    foodCnt++;
                    continue;
                }
                // 没有农田可种（动物/浆果耗尽）→ 建农田保食物供给（需市场+木头，建在谷仓旁）
                if (countBuilding(info, BUILDING_MARKET) > 0
                    && countBuilding(info, BUILDING_FARM) < targetFood
                    && info.Wood >= BUILD_FARM_WOOD) {
                    // 找谷仓位置（默认市中心）
                    int gx = m_centerX, gy = m_centerY;
                    for (const tagBuilding& b : info.buildings) {
                        if (b.Type == BUILDING_GRANARY) { gx = b.BlockDR; gy = b.BlockUR; break; }
                    }
                    int x, y;
                    if (findBuildBlock(info, x, y, 3, 3, gx, gy)) {
                        HumanBuild(f.SN, BUILDING_FARM, x, y);
                        m_issued.insert(f.SN);
                        continue;
                    }
                }
            }
        }
        // ⑤ 黄金（铜器后）；专属食物采集者不挖金
        if (!isFood && goldCnt < targetGold) {
            int sn = findNearestResource(info, RESOURCE_GOLD, f.SN);
            if (sn >= 0) {
                HumanAction(f.SN, sn);
                m_issued.insert(f.SN);
                goldCnt++;
                continue;
            }
        }
        // ⑦ 兜底：农民不闲置
        //    普通农民：打猎或砍树
        //    专属食物采集者：先找安全食物（打猎/采尸）；打不了（如大象人数不足/猎物满员）
        //    → 也去砍树/挖资源——宁可干别的也不站着等（防"空闲村民不动"）
        int sn = findNearestHunt(info, f.SN);
        if (sn < 0) sn = findNearestResource(info, RESOURCE_TREE, f.SN);
        if (sn >= 0) {
            HumanAction(f.SN, sn);
            m_issued.insert(f.SN);
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

// 统计我方某兵种数量
int UsrAI::countArmy(const tagInfo& info, int sort) const
{
    int cnt = 0;
    for (const tagArmy& a : info.armies)
        if (a.Sort == sort) cnt++;
    return cnt;
}

// ============================================================
// 市镇中心：升级铜器（优先）→ 分阶段生产农民
// 农民节奏：第一波前 12（开局8+新4采浆果）→ 第二波前 16（再新4打猎）→ 之后 20
// 升级条件：市场/靶场/马厩 已建 2 个 + 800 食物（升级优先，保证按时升铜器）
// ============================================================
void UsrAI::manageCenter(const tagInfo& info)
{
    // 分阶段农民目标：第一波前 12，第二波前 16，之后 20（配合专属食物采集方案）
    int farmerTarget = TARGET_FARMER_NUM;
    if (info.GameFrame < FRAME_WAVE1) farmerTarget = 12;
    else if (info.GameFrame < FRAME_WAVE2) farmerTarget = 16;

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
            return;     // 本帧中心只做一件事
        }
        // 2) 生产农民：仅当升级建筑还没建齐时补农民（建齐后停补，全力攒 800 食物升级）
        //    升级后不再造农民，食物/黄金全力投入造兵防守第二波
        bool upgradeBuildingReady = (info.civilizationStage < CIVILIZATION_BRONZEAGE
                                     && canUpgradeBronze(info));
        if (!upgradeBuildingReady
            && info.civilizationStage < CIVILIZATION_BRONZEAGE
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
// 并行建造：最多 2 个农民同时建（专职建造者 + 抽调一个空闲农民），加速进度
// ============================================================
void UsrAI::buildBuildings(const tagInfo& info)
{
    for (int round = 0; round < 2; ++round) {
        // 找空闲建造者：第一轮专职建造者，第二轮任意空闲农民（抽调）
        int builder = -1;
        if (round == 0) {
            if (m_builderSN >= 0) {
                for (const tagFarmer& f : info.farmers) {
                    if (f.SN == m_builderSN) {
                        if (f.NowState == HUMAN_STATE_IDLE && !m_issued.count(f.SN)) builder = f.SN;
                        break;
                    }
                }
            }
        } else {
            for (const tagFarmer& f : info.farmers) {
                if (f.FarmerSort != FARMERTYPE_FARMER) continue;
                if (f.NowState != HUMAN_STATE_IDLE) continue;
                if (m_issued.count(f.SN)) continue;
                if (f.SN == m_builderSN) continue;          // 第一轮已用专职建造者
                if (f.SN == m_depotBuilderSN) continue;     // 资源点建造者不抽调（专职建仓库/谷仓）
                builder = f.SN;
                break;
            }
        }
        if (builder == -1) break;

        bool built = false;
        // 1) 住房（5座，先拉人口到 20 农民）
        if (countBuilding(info, BUILDING_HOME) < TARGET_HOUSE_NUM && info.Wood >= BUILD_HOUSE_WOOD) {
            int x, y;
            if (findBuildBlock(info, x, y, 2, 2)) {
                HumanBuild(builder, BUILDING_HOME, x, y);
                m_issued.insert(builder);
                built = true;
            }
        }
        // 2) 箭塔（住房满后建 1 座，补防御；第二、三座由 buildArrowTower 紧凑建造）
        else if (countBuilding(info, BUILDING_HOME) >= TARGET_HOUSE_NUM
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
        // 3) 兵营（练兵防守）
        else if (countBuilding(info, BUILDING_ARMYCAMP) == 0 && info.Wood >= BUILD_ARMYCAMP_WOOD) {
            int x, y;
            if (findBuildBlock(info, x, y, 3, 3)) {
                HumanBuild(builder, BUILDING_ARMYCAMP, x, y);
                m_issued.insert(builder);
                built = true;
            }
        }
        // 4) 市场（升级必需！升级前必须造出来）
        else if (countBuilding(info, BUILDING_MARKET) == 0 && info.Wood >= BUILD_MARKET_WOOD) {
            int x, y;
            if (findBuildBlock(info, x, y, 3, 3)) {
                HumanBuild(builder, BUILDING_MARKET, x, y);
                m_issued.insert(builder);
                built = true;
            }
        }
        // 5) 靶场（需兵营+市场；升级必需：市场+靶场 = 2 个工具时代建筑 → 可升级）
        else if (countBuilding(info, BUILDING_MARKET) > 0
            && countBuilding(info, BUILDING_ARMYCAMP) > 0
            && countBuilding(info, BUILDING_RANGE) == 0 && info.Wood >= BUILD_RANGE_WOOD) {
            int x, y;
            if (findBuildBlock(info, x, y, 3, 3)) {
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
            && countBuilding(info, BUILDING_FARM) < (int)info.farmers.size() / 2
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
        // （羚羊堆仓库/浆果堆谷仓由采集者负责，见 buildResourceDepots）
        if (!built) break;   // 无可建建筑，不再找第二个农民
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
    // ---- 判断是否需要建仓 ----
    // 1) 羚羊堆旁仓库（猎物 ≥3 且仓库不足 2）
    int animalCnt = 0;
    double ax = 0, ay = 0;
    for (const tagResource& r : info.resources) {
        if (r.Type != RESOURCE_GAZELLE && r.Type != RESOURCE_ELEPHANT && r.Type != RESOURCE_LION) continue;
        if (r.Cnt <= 0) continue;
        animalCnt++;
        ax += r.DR;
        ay += r.UR;
    }
    bool needStock = (animalCnt >= 3 && countBuilding(info, BUILDING_STOCK) < 2
                      && info.Wood >= BUILD_STOCK_WOOD);
    // 2) 浆果堆旁谷仓（浆果 ≥3 且离现有谷仓远）
    int berryCnt2 = 0;
    double bx = 0, by = 0;
    for (const tagResource& r : info.resources) {
        if (r.Type != RESOURCE_BUSH || r.Cnt <= 0) continue;
        berryCnt2++;
        bx += r.DR;
        by += r.UR;
    }
    bool needGranary = false;
    if (berryCnt2 >= 3 && countBuilding(info, BUILDING_GRANARY) < 2
        && info.Wood >= BUILD_GRANARY_WOOD) {
        double maxBerryDist = 0;
        for (const tagResource& r : info.resources) {
            if (r.Type != RESOURCE_BUSH || r.Cnt <= 0) continue;
            double minD = 1e18;
            for (const tagBuilding& b : info.buildings) {
                if (b.Type != BUILDING_GRANARY || b.Percent < 100) continue;
                double d = calDistance(r.DR, r.UR,
                                       (double)b.BlockDR * BLOCKSIDELENGTH, (double)b.BlockUR * BLOCKSIDELENGTH);
                if (d < minD) minD = d;
            }
            if (minD > maxBerryDist) maxBerryDist = minD;
        }
        needGranary = (maxBerryDist > 6.0 * BLOCKSIDELENGTH);
    }
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

    // ---- 空闲状态：优先派他去建仓库/谷仓 ----
    if (needStock) {
        int gx = (int)(ax / animalCnt / BLOCKSIDELENGTH);
        int gy = (int)(ay / animalCnt / BLOCKSIDELENGTH);
        int x, y;
        if (findBuildBlock(info, x, y, 3, 3, gx, gy)) {
            HumanBuild(m_depotBuilderSN, BUILDING_STOCK, x, y);
            m_issued.insert(m_depotBuilderSN);
            return;
        }
    }
    if (needGranary) {
        int gx = (int)(bx / berryCnt2 / BLOCKSIDELENGTH);
        int gy = (int)(by / berryCnt2 / BLOCKSIDELENGTH);
        int x, y;
        if (findBuildBlock(info, x, y, 3, 3, gx, gy)) {
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
    for (const tagBuilding& b : info.buildings) {
        if (b.Percent < 100 || b.Project != ACT_NULL) continue;   // 建造中或忙碌
        if (m_issued.count(b.SN)) continue;                        // 本帧已下令
        if (savingForUpgrade) {
            // 攒升级期间：只做谷仓的箭塔科技（其余一律暂停）
            if (b.Type == BUILDING_GRANARY
                && m_researchCount[BUILDING_GRANARY_ARROWTOWER] == 0
                && info.Meat >= BUILDING_GRANARY_ARROWTOWER_FOOD) {
                BuildingAction(b.SN, BUILDING_GRANARY_ARROWTOWER);
                m_issued.insert(b.SN);
                m_researchCount[BUILDING_GRANARY_ARROWTOWER]++;
            }
            continue;
        }
        switch (b.Type) {
        case BUILDING_GRANARY: {
            // 解锁箭塔（工具时代）→ 升级箭塔（铜器）
            if (m_researchCount[BUILDING_GRANARY_ARROWTOWER] == 0
                && info.Meat >= BUILDING_GRANARY_ARROWTOWER_FOOD) {
                BuildingAction(b.SN, BUILDING_GRANARY_ARROWTOWER);
                m_issued.insert(b.SN);
                m_researchCount[BUILDING_GRANARY_ARROWTOWER]++;
                break;
            }
            if (bronze && m_researchCount[BUILDING_GRANARY_ARROWTOWE_UPGRADE] == 0
                && info.Meat >= BUILDING_GRANARY_UPGRADE_ARROWTOWER_FOOD
                && info.Stone >= BUILDING_GRANARY_UPGRADE_ARROWTOWER_STONE) {
                BuildingAction(b.SN, BUILDING_GRANARY_ARROWTOWE_UPGRADE);
                m_issued.insert(b.SN);
                m_researchCount[BUILDING_GRANARY_ARROWTOWE_UPGRADE]++;
                break;
            }
            break;
        }
        case BUILDING_MARKET: {
            // 伐木 → 采石（前期采集加速）→ 车轮（铜器）→ 采金
            if (m_researchCount[BUILDING_MARKET_WOOD_UPGRADE] == 0
                && info.Meat >= BUILDING_MARKET_WOOD_UPGRADE_FOOD
                && info.Wood >= BUILDING_MARKET_WOOD_UPGRADE_WOOD) {
                BuildingAction(b.SN, BUILDING_MARKET_WOOD_UPGRADE);
                m_issued.insert(b.SN);
                m_researchCount[BUILDING_MARKET_WOOD_UPGRADE]++;
                break;
            }
            // 采石（石头采集加速 → 箭塔/升级更快）
            if (m_researchCount[BUILDING_MARKET_STONE_UPGRADE] == 0
                && info.Meat >= BUILDING_MARKET_STONE_UPGRADE_FOOD
                && info.Stone >= BUILDING_MARKET_STONE_UPGRADE_STONE) {
                BuildingAction(b.SN, BUILDING_MARKET_STONE_UPGRADE);
                m_issued.insert(b.SN);
                m_researchCount[BUILDING_MARKET_STONE_UPGRADE]++;
                break;
            }
            if (bronze && m_researchCount[BUILDING_MARKET_WHEEL_UPGRADE] == 0
                && info.Meat >= BUILDING_MARKET_WHEEL_UPGRADE_FOOD
                && info.Wood >= BUILDING_MARKET_WHEEL_UPGRADE_WOOD) {
                BuildingAction(b.SN, BUILDING_MARKET_WHEEL_UPGRADE);
                m_issued.insert(b.SN);
                m_researchCount[BUILDING_MARKET_WHEEL_UPGRADE]++;
                break;
            }
            if (m_researchCount[BUILDING_MARKET_GOLD_UPGRADE] == 0
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
            if (m_researchCount[BUILDING_STOCK_UPGRADE_DEFENSE_INFANTRY] == 0
                && info.Meat >= BUILDING_STOCK_UPGRADE_DEFENSE_INFANTRY_FOOD) {
                BuildingAction(b.SN, BUILDING_STOCK_UPGRADE_DEFENSE_INFANTRY);
                m_issued.insert(b.SN);
                m_researchCount[BUILDING_STOCK_UPGRADE_DEFENSE_INFANTRY]++;
                break;
            }
            // 弓兵护甲
            if (m_researchCount[BUILDING_STOCK_UPGRADE_DEFENSE_ARCHER] == 0
                && info.Meat >= BUILDING_STOCK_UPGRADE_DEFENSE_ARCHER_FOOD) {
                BuildingAction(b.SN, BUILDING_STOCK_UPGRADE_DEFENSE_ARCHER);
                m_issued.insert(b.SN);
                m_researchCount[BUILDING_STOCK_UPGRADE_DEFENSE_ARCHER]++;
                break;
            }
            // 骑兵护甲
            if (m_researchCount[BUILDING_STOCK_UPGRADE_DEFENSE_RIDER] == 0
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
            // 铜器后优先阔剑兵（需科技），否则棍棒兵
            if (bronze && m_researchCount[BUILDING_ARMYCAMP_UPGRADE_BROADSWORD] > 0
                && info.Meat >= BUILDING_ARMYCAMP_CREATE_BROADSWORD_FOOD && info.Gold >= 15) {
                BuildingAction(b.SN, BUILDING_ARMYCAMP_CREATE_BROADSWORD);
                m_issued.insert(b.SN);
            } else if (info.GameFrame < FRAME_WAVE2 && countArmy(info, AT_CLUBMAN) >= 2) {
                // 第二波前棍棒兵只留 2 个：省食物给升级铜器/铜器兵，防止造太多吃光食物
            } else if (info.Meat >= BUILDING_ARMYCAMP_CREATE_CLUBMAN_FOOD) {
                BuildingAction(b.SN, BUILDING_ARMYCAMP_CREATE_CLUBMAN);
                m_issued.insert(b.SN);
            }
            break;
        case BUILDING_RANGE:
            // 铜器后优先复合弓兵（需科技），否则弓箭手
            if (bronze && m_researchCount[BUILDING_RANGE_UPGRADE_COMPOSITE_BOW] > 0
                && info.Meat >= BUILDING_RANGE_CREATE_COMPOSITE_BOWMAN_FOOD && info.Gold >= 20) {
                BuildingAction(b.SN, BUILDING_RANGE_CREATE_COMPOSITE_BOWMAN);
                m_issued.insert(b.SN);
            } else if (info.Meat >= BUILDING_RANGE_CREATE_BOWMAN_FOOD && info.Wood >= 20) {
                BuildingAction(b.SN, BUILDING_RANGE_CREATE_BOWMAN);
                m_issued.insert(b.SN);
            }
            break;
        case BUILDING_STABLE:
            // 保持 1 个侦察骑兵探路，其余铜器后造骑兵
            if (countArmy(info, AT_SCOUT) < 1 && info.Meat >= BUILDING_STABLE_CREATE_SCOUT_FOOD) {
                BuildingAction(b.SN, BUILDING_STABLE_CREATE_SCOUT);
                m_issued.insert(b.SN);
            } else if (bronze && info.Meat >= BUILDING_STABLE_CREATE_CAVALRY_FOOD && info.Gold >= 80) {
                BuildingAction(b.SN, BUILDING_STABLE_CREATE_CAVALRY);
                m_issued.insert(b.SN);
            }
            break;
        case BUILDING_COLLAGE:
            // 方阵兵（铜器强力步兵）
            if (info.Meat >= BUILDING_COLLAGE_CREATE_HOPLITE_FOOD && info.Gold >= 40) {
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
    if (towerCount >= 3 || towerCount == 0) return;   // 已有三座 / 第一座还没建
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
        if (a.NowState != HUMAN_STATE_IDLE) continue;               // 已在战斗的不重复下令
        if (m_issued.count(a.SN)) continue;                         // 本帧已下令

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
    // ② 节流：60帧内且目标没大变 → 不下令
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
    //      走位条件：有军队 +（血量低 <60% 或 没在转化）
    //      转化中血量健康时不打断（转化受攻击不会自动断，是我们的走位打断了它）
    bool beingHit = (m_priestLastBlood > 0 && priest->Blood < m_priestLastBlood);
    m_priestLastBlood = priest->Blood;
    bool convertingNow = false;
    for (const tagArmy& e : info.enemy_armies)
        if (e.SN == priest->WorkObjectSN) { convertingNow = true; break; }
    bool lowBlood = (priest->Blood < priest->MaxBlood * 3 / 5);
    if (beingHit && !info.enemy_armies.empty() && (lowBlood || !convertingNow)) {
        // 走位目标 = 固定安全位（塔下/市中心，getPriestHome）：挨打就往安全位撤，到位即停
        // （不用"塔+敌人反方向偏移"——敌人位置每帧变 → 目标抖动 → 每帧重新下令）
        int hx, hy;
        getPriestHome(info, hx, hy);
        double gx = (double)hx * BLOCKSIDELENGTH;
        double gy = (double)hy * BLOCKSIDELENGTH;
        // 节流下令：60帧内目标不变不重复下令（防每帧打断移动/挤掉转化指令）
        bool ordered = movePriest(priestSN, priest->DR, priest->UR, gx, gy, info.GameFrame);
        if (ordered || lowBlood) return;   // 已下令，或血量低（保命优先）→ 本帧不再转化
        // 血量健康且节流期内：不 return → 把本帧让给转化逻辑
    }

    // 2) 寻找转化候选
    //    优先远程兵（弓箭手/战车弓兵/复合弓兵/投石兵）：它们打祭司最危险且塔可能够不着，
    //    祭司转化射程12格能覆盖（弓箭手射程5、战车弓兵7）→ 优先转化保命
    //    保守阶段（未升级）：没有远程兵 → 塔射程内最近敌人
    //    激进阶段（升级后）：没有远程兵 → 最近敌人
    bool aggressive = (info.civilizationStage >= CIVILIZATION_BRONZEAGE
                       || info.GameFrame > FRAME_WAVE1 + 3000);
    int target = -1;

    // ① 优先远程兵（选最近的）
    double bestR = 1e18;
    for (const tagArmy& e : info.enemy_armies) {
        if (e.Blood <= 0) continue;
        if (e.Sort != AT_BOWMAN && e.Sort != AT_CHARIOT_ARCHER
            && e.Sort != AT_COMPOSITE_BOWMAN && e.Sort != AT_SLINGER) continue;
        double d = calDistance(priest->DR, priest->UR, e.DR, e.UR);
        if (d < bestR) { bestR = d; target = e.SN; }
    }
    if (target >= 0) {
        // 已选到远程兵
    } else if (aggressive) {
        // ② 激进：无远程兵 → 最近敌人
        double best = 1e18;
        for (const tagArmy& e : info.enemy_armies) {
            if (e.Blood <= 0) continue;
            double d = calDistance(priest->DR, priest->UR, e.DR, e.UR);
            if (d < best) { best = d; target = e.SN; }
        }
    } else {
        // ② 保守：无远程兵 → 塔射程内最近敌人
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
        bool tooSoon = (m_convertStartFrame >= 0 && info.GameFrame - m_convertStartFrame < 120);
        needConvertOrder = !tooSoon;   // 120帧内不再下令，转化持续完成
    }

    // 3) 有转化目标且节流通过 → 主动转化（敌人打别人时也转化，不等敌人打自己）
    if (needConvertOrder) {
        HumanAction(priestSN, target);
        m_issued.insert(priestSN);
        m_convertTarget = target;
        m_convertStartFrame = info.GameFrame;
        return;
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
    if (threat != nullptr && nearest < threatDist) {
        int hx, hy;
        getPriestHome(info, hx, hy);
        if (hx >= 0) {
            movePriest(priestSN, priest->DR, priest->UR, (double)hx * BLOCKSIDELENGTH, (double)hy * BLOCKSIDELENGTH, info.GameFrame);
        }
        return;
    }

    // 6) 无威胁且无转化目标：不在塔下 → 回塔下待命（节流下令）
    if (!nearTower && info.GameFrame > FRAME_WAVE1 - 2000) {
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
}
