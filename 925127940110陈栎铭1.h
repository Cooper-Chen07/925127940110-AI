#ifndef USRAI_H
#define USRAI_H

#include "ai.h"
#include <unordered_map>
#include <set>

extern tagGame tagUsrGame;
extern ins UsrIns;
/*##########DO NOT MODIFY THE CODE ABOVE##########*/

class UsrAI:public AI
{
public:
    UsrAI(){this->id=0;}
    ~UsrAI(){}

private:
    void processData() override;
    tagInfo getInfo(){return tagUsrGame.getInfo();}
    int AddToIns(instruction ins) override
    {
        UsrIns.lock.lock();
        ins.id=UsrIns.g_id;
        UsrIns.g_id++;
        UsrIns.instructions.push(ins);
        UsrIns.lock.unlock();
        return ins.id;
    }
    void clearInsRet() override
    {
        tagUsrGame.clearInsRet();
    }
    /*##########DO NOT MODIFY THE CODE IN THE CLASS##########*/

    // ===== 祭司探路（跨帧状态） =====
    int m_scoutIdx = 0;                              // 已完成探路次数（跨帧保存）
    int m_scoutStartFrame = -1;                      // 探路下令帧（卡住超时判断用）
    int m_centerX = -1, m_centerY = -1;              // 市镇中心块坐标（回家/找地参照）
    void scoutWithPriest(const tagInfo& info);       // 祭司探路（前期，视野12）
    void scoutWithScout(const tagInfo& info);        // 侦察骑兵探路（后期，视野8速度快的远探）

    // ===== 地图（PPT 阶段1算法框架） =====
    int m_map[100][100] = {};                    // 地图标记：0=已探明空地 >0=占用 <0=不可走
    std::vector<Point> m_explored;               // 已探明的空地集合（用于探路等）
    int m_searchX = 0, m_searchY = 0;            // 找建筑空地的搜索起点
    void updateMap(const tagInfo& info);         // 建立地图标记数组
    void markBlock(int bx, int by, int size, int val);   // 标记一片占用区域
    bool findBuildBlock(const tagInfo& info, int& x, int& y, int w, int h, int nearX = -1, int nearY = -1); // 找 w×h 可建空地（可指定附近位置）

    // ===== 经济：农民工作分配 =====
    static const int MAX_HUNTER_PER_PREY = 2;     // 每只活物最多猎人（两两一组分散猎杀）
    std::set<int> m_foodGatherers;               // 专属食物采集者（浆果/打猎）：只做食物，干完自动找下一个食物资源
    std::set<int> m_issued;                      // 本帧已下令的对象 SN（防重复下令）
    int m_builderSN = -1;                        // 专职建造村民 SN（保证建筑能建起来）
    std::unordered_map<int,int> m_moveStart;     // 农民SN -> 开始移动帧（寻路卡住检测）
    void manageVillagers(const tagInfo& info);   // 农民分配：食物优先（浆果/打猎/种田）动态配额
    int  findNearestResource(const tagInfo& info, int type, int farmerSN); // 找最近指定资源
    int  findNearestHunt(const tagInfo& info, int farmerSN);               // 打猎：随机选目标（尸体优先，防扎堆）
    int  findNearestFarm(const tagInfo& info, int farmerSN);               // 找最近可种农田
    int  countBuilding(const tagInfo& info, int type) const;                     // 统计已建成建筑数
    int  countArmy(const tagInfo& info, int sort) const;                         // 统计我方某兵种数量

    // ===== 生产与科技 =====
    std::unordered_map<int,int> m_researchCount; // 科技 Action -> 已发起次数（防重复研发）
    void manageCenter(const tagInfo& info);      // 市镇中心：升级铜器（优先）→ 生产农民到 20
    void researchTech(const tagInfo& info);      // 科技链：谷仓/市场/仓库/兵营/靶场
    bool canUpgradeBronze(const tagInfo& info) const;  // 市场/马厩/靶场是否已建够 2 个
    void buildBuildings(const tagInfo& info);    // 建筑规划：住房→箭塔→兵营→市场→靶场→马厩→学院→农田
    void buildResourceDepots(const tagInfo& info); // 资源点仓库/谷仓：由采集者负责（羚羊堆/浆果堆）
    void trainArmy(const tagInfo& info);         // 训练军队：棍棒→阔剑 / 弓箭→复合弓 / 侦察骑→骑兵 / 方阵兵

    // ===== 防守 =====
    void defense(const tagInfo& info);           // 箭塔主动攻击射程内的敌人
    void handlePriest(const tagInfo& info);      // 祭司：被敌人威胁时撤退到双塔中点（拉怪）
    int m_convertTarget = -1;                    // 上次转化目标 SN（转化节流用）
    int m_convertStartFrame = -1;                // 上次转化下令帧（转化节流用）
    int m_priestLastBlood = -1;                  // 祭司上一帧血量（被攻击检测用）
    int m_priestMoveFrame = -9999;               // 上次祭司移动下令帧（移动节流用）
    double m_priestMoveDR = 0, m_priestMoveUR = 0; // 上次移动目标（移动节流用）
    bool movePriest(int priestSN, double px, double py, double gx, double gy, int frame); // 祭司节流移动：到位即停+60帧节流
    bool isStaticBlock(int bx, int by) const;                 // 某格是否静态障碍（树/建筑/海洋；单位不算）
    void adjustReachableTarget(double& gx, double& gy) const; // 目标格是障碍 → 调整到最近可达格
    void buildArrowTower(const tagInfo& info);   // 建造第二座箭塔（与第一座形成交叉火力）
    void getPriestHome(const tagInfo& info, int& hx, int& hy) const; // 祭司站位：双塔中点>单塔>市中心
    std::unordered_map<int,int> m_towerSwitch;   // 箭塔SN -> 上次下令帧（切换节流，防频繁切换不射击）
    int m_enemyDirX = 0, m_enemyDirY = 0;        // 敌人来袭方向（相对市中心，±1）


};

/*##########YOUR CODE BEGINS HERE##########*/




/*##########YOUR CODE ENDS HERE##########*/
#endif // USRAI_H
