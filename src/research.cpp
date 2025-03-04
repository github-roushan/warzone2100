/*
	This file is part of Warzone 2100.
	Copyright (C) 1999-2004  Eidos Interactive
	Copyright (C) 2005-2020  Warzone 2100 Project

	Warzone 2100 is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	Warzone 2100 is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Warzone 2100; if not, write to the Free Software
	Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
*/
/*
 * Research.c
 *
 * Research tree and functions!
 *
 */
#include <string.h>
#include <map>

#include "lib/framework/frame.h"
#include "lib/netplay/sync_debug.h"
#include "lib/ivis_opengl/imd.h"
#include "objects.h"
#include "lib/gamelib/gtime.h"
#include "research.h"
#include "message.h"
#include "lib/sound/audio.h"
#include "lib/sound/audio_id.h"
#include "hci.h"
#include "console.h"
#include "cmddroid.h"
#include "power.h"
#include "mission.h"
#include "frend.h"		// frontend ids.
#include "intimage.h"
#include "multiplay.h"
#include "template.h"
#include "qtscript.h"
#include "stats.h"
#include "wzapi.h"

// The stores for the research stats
std::vector<RESEARCH> asResearch;
optional<ResearchUpgradeCalculationMode> researchUpgradeCalcMode;
std::unordered_map<WzString, std::vector<size_t>> resCategories;
nlohmann::json cachedStatsObject = nlohmann::json(nullptr);
std::vector<wzapi::PerPlayerUpgrades> cachedPerPlayerUpgrades;

typedef std::unordered_map<std::string, std::unordered_map<std::string, int64_t>> RawResearchUpgradeChangeValues;
std::array<RawResearchUpgradeChangeValues, MAX_PLAYERS> cachedPerPlayerRawUpgradeChange;

//used for Callbacks to say which topic was last researched
RESEARCH                *psCBLastResearch;
STRUCTURE				*psCBLastResStructure;
SDWORD					CBResFacilityOwner;

//List of pointers to arrays of PLAYER_RESEARCH[numResearch] for each player
std::vector<PLAYER_RESEARCH> asPlayerResList[MAX_PLAYERS];

/* Default level of sensor, Repair and ECM */
UDWORD					aDefaultSensor[MAX_PLAYERS];
UDWORD					aDefaultECM[MAX_PLAYERS];
UDWORD					aDefaultRepair[MAX_PLAYERS];

// Per-player statistics about research upgrades
struct PlayerUpgradeCounts
{
	std::unordered_map<std::string, uint32_t> numBodyClassArmourUpgrades;
	std::unordered_map<std::string, uint32_t> numBodyClassThermalUpgrades;
	std::unordered_map<std::string, uint32_t> numWeaponImpactClassUpgrades;

	// helper functions
	uint32_t getNumWeaponImpactClassUpgrades(WEAPON_SUBCLASS subClass);
	uint32_t getNumBodyClassArmourUpgrades(BodyClass bodyClass);
	uint32_t getNumBodyClassThermalArmourUpgrades(BodyClass bodyClass);
};
std::vector<PlayerUpgradeCounts> playerUpgradeCounts;

//set the iconID based on the name read in in the stats
static UWORD setIconID(const char *pIconName, const char *pName);
static void replaceComponent(COMPONENT_STATS *pNewComponent, COMPONENT_STATS *pOldComponent,
                             UBYTE player);
static bool checkResearchName(RESEARCH *psRes, UDWORD numStats);

//flag that indicates whether the player can self repair
static UBYTE bSelfRepair[MAX_PLAYERS];
static void replaceDroidComponent(DroidList& pList, UDWORD oldType, UDWORD oldCompInc,
                                  UDWORD newCompInc);
static void replaceStructureComponent(StructureList& pList, UDWORD oldType, UDWORD oldCompInc,
                                      UDWORD newCompInc, UBYTE player);
static void switchComponent(DROID *psDroid, UDWORD oldType, UDWORD oldCompInc,
                            UDWORD newCompInc);

static void replaceTransDroidComponents(DROID *psTransporter, UDWORD oldType,
                                        UDWORD oldCompInc, UDWORD newCompInc);


bool researchInitVars()
{
	psCBLastResearch = nullptr;
	psCBLastResStructure = nullptr;
	CBResFacilityOwner = -1;
	asResearch.clear();
	researchUpgradeCalcMode = nullopt;
	resCategories.clear();
	cachedStatsObject = nlohmann::json(nullptr);
	cachedPerPlayerUpgrades.clear();
	playerUpgradeCounts = std::vector<PlayerUpgradeCounts>(MAX_PLAYERS);

	for (int i = 0; i < MAX_PLAYERS; i++)
	{
		bSelfRepair[i] = false;
		aDefaultSensor[i] = 0;
		aDefaultECM[i] = 0;
		aDefaultRepair[i] = 0;
	}

	return true;
}

ResearchUpgradeCalculationMode getResearchUpgradeCalcMode()
{
	// Default to ResearchUpgradeCalculationMode::Compat, unless otherwise specified
	return researchUpgradeCalcMode.value_or(ResearchUpgradeCalculationMode::Compat);
}

uint32_t PlayerUpgradeCounts::getNumWeaponImpactClassUpgrades(WEAPON_SUBCLASS subClass)
{
	auto subClassStr = getWeaponSubClass(subClass);
	auto it = numWeaponImpactClassUpgrades.find(subClassStr);
	if (it == numWeaponImpactClassUpgrades.end())
	{
		return 0;
	}
	return it->second;
}

static inline const char* bodyClassToStr(BodyClass bodyClass)
{
	const char* bodyClassStr = nullptr;
	switch (bodyClass)
	{
		case BodyClass::Tank:
			bodyClassStr = "Droids";
			break;
		case BodyClass::Cyborg:
			bodyClassStr = "Cyborgs";
			break;
	}
	return bodyClassStr;
}

uint32_t PlayerUpgradeCounts::getNumBodyClassArmourUpgrades(BodyClass bodyClass)
{
	const char* bodyClassStr = bodyClassToStr(bodyClass);
	auto it = numBodyClassArmourUpgrades.find(bodyClassStr);
	if (it == numBodyClassArmourUpgrades.end())
	{
		return 0;
	}
	return it->second;
}

uint32_t PlayerUpgradeCounts::getNumBodyClassThermalArmourUpgrades(BodyClass bodyClass)
{
	const char* bodyClassStr = bodyClassToStr(bodyClass);
	auto it = numBodyClassThermalUpgrades.find(bodyClassStr);
	if (it == numBodyClassThermalUpgrades.end())
	{
		return 0;
	}
	return it->second;
}

uint32_t getNumWeaponImpactClassUpgrades(uint32_t player, WEAPON_SUBCLASS subClass)
{
	ASSERT_OR_RETURN(0, player < playerUpgradeCounts.size(), "Out of bounds player: %" PRIu32 "", player);
	return playerUpgradeCounts[player].getNumWeaponImpactClassUpgrades(subClass);
}

uint32_t getNumBodyClassArmourUpgrades(uint32_t player, BodyClass bodyClass)
{
	ASSERT_OR_RETURN(0, player < playerUpgradeCounts.size(), "Out of bounds player: %" PRIu32 "", player);
	return playerUpgradeCounts[player].getNumBodyClassArmourUpgrades(bodyClass);
}

uint32_t getNumBodyClassThermalArmourUpgrades(uint32_t player, BodyClass bodyClass)
{
	ASSERT_OR_RETURN(0, player < playerUpgradeCounts.size(), "Out of bounds player: %" PRIu32 "", player);
	return playerUpgradeCounts[player].getNumBodyClassThermalArmourUpgrades(bodyClass);
}

class CycleDetection
{
private:
	CycleDetection() {}

	std::unordered_set<RESEARCH *> visited;
	std::unordered_set<RESEARCH *> exploring;

	nonstd::optional<std::deque<RESEARCH *>> explore(RESEARCH *research)
	{
		if (visited.find(research) != visited.end())
		{
			return nonstd::nullopt;
		}

		if (exploring.find(research) != exploring.end())
		{
			return {{research}};
		}

		exploring.insert(research);

		for (auto requirementIndex: research->pPRList)
		{
			auto requirement = &asResearch[requirementIndex];
			if (auto cycle = explore(requirement))
			{
				cycle->push_front(research);
				return cycle;
			}
		}

		exploring.erase(exploring.find(research));
		visited.insert(research);
		return nonstd::nullopt;
	}

public:
	static nonstd::optional<std::deque<RESEARCH *>> detectCycle()
	{
		CycleDetection detection;

		for (auto &research: asResearch)
		{
			if (auto cycle = detection.explore(&research))
			{
				return cycle;
			}
		}

		return nonstd::nullopt;
	}
};

static bool isResAPrereqForResB(size_t resAIndex, size_t resBIndex)
{
	if (resAIndex == resBIndex)
	{
		return false;
	}
	const RESEARCH *resB = &asResearch[resBIndex];
	std::deque<const RESEARCH *> stack = {resB};
	while (!stack.empty())
	{
		const RESEARCH *pCurr = stack.back();
		stack.pop_back();
		for (auto prereqIndex: pCurr->pPRList)
		{
			if (prereqIndex == resAIndex)
			{
				return true;
			}
			auto prereq = &asResearch[prereqIndex];
			stack.push_back(prereq);
		}
	}
	return false;
}

static optional<ResearchUpgradeCalculationMode> resCalcModeStringToValue(const WzString& calcModeStr)
{
	if (calcModeStr.compare("compat") == 0)
	{
		return ResearchUpgradeCalculationMode::Compat;
	}
	else if (calcModeStr.compare("improved") == 0)
	{
		return ResearchUpgradeCalculationMode::Improved;
	}
	else
	{
		return nullopt;
	}
}

static const char* resCalcModeToString(ResearchUpgradeCalculationMode mode)
{
	switch (mode)
	{
		case ResearchUpgradeCalculationMode::Compat:
			return "compat";
		case ResearchUpgradeCalculationMode::Improved:
			return "improved";
	}
	return "invalid";
}

#define RESEARCH_JSON_CONFIG_DICT_KEY "_config_"

/** Load the research stats */
bool loadResearch(WzConfig &ini)
{
	ASSERT(ini.isAtDocumentRoot(), "WzConfig instance is in the middle of traversal");
	const WzString CONFIG_DICT_KEY_STR = RESEARCH_JSON_CONFIG_DICT_KEY;
	std::vector<WzString> list = ini.childGroups();
	PLAYER_RESEARCH dummy;
	memset(&dummy, 0, sizeof(dummy));
	std::vector<std::vector<WzString>> preResearch;
	preResearch.resize(list.size());
	for (size_t inc = 0; inc < list.size(); ++inc)
	{
		if (list[inc] == CONFIG_DICT_KEY_STR)
		{
			// handle the special config dict
			ini.beginGroup(list[inc]);

			// calculationMode
			auto calcModeStr = ini.value("calculationMode", resCalcModeToString(ResearchUpgradeCalculationMode::Compat)).toWzString();
			auto calcModeParsed = resCalcModeStringToValue(calcModeStr);
			if (calcModeParsed.has_value())
			{
				if (!researchUpgradeCalcMode.has_value())
				{
					researchUpgradeCalcMode = calcModeParsed.value();
				}
				else
				{
					if (researchUpgradeCalcMode.value() != calcModeParsed.value())
					{
						debug(LOG_ERROR, "Non-matching research JSON calculationModes");
						debug(LOG_INFO, "Research JSON file \"%s\" has specified a calculationMode (\"%s\") that does not match the first loaded research JSON's calculationMode (\"%s\")", ini.fileName().toUtf8().c_str(), calcModeStr.toUtf8().c_str(), resCalcModeToString(researchUpgradeCalcMode.value()));
					}
				}
			}
			else
			{
				ASSERT_OR_RETURN(false, false, "Invalid _config_ \"calculationMode\" value: \"%s\"", calcModeStr.toUtf8().c_str());
			}

			ini.endGroup();
			continue;
		}

		// HACK FIXME: the code assumes we have empty PLAYER_RESEARCH entries to throw around
		for (auto &j : asPlayerResList)
		{
			j.push_back(dummy);
		}

		ini.beginGroup(list[inc]);
		RESEARCH research;
		research.index = inc;
		research.name = ini.string("name");
		research.category = ini.string("category");
		research.id = list[inc];

		//check the name hasn't been used already
		ASSERT_OR_RETURN(false, checkResearchName(&research, inc), "Research name '%s' used already", getStatsName(&research));

		research.ref = STAT_RESEARCH + inc;

		research.results = ini.json("results", nlohmann::json::array());

		//set subGroup icon
		WzString subGroup = ini.value("subgroupIconID", "").toWzString();
		if (subGroup.compare("") != 0)
		{
			research.subGroup = setIconID(subGroup.toUtf8().c_str(), getStatsName(&research));
		}
		else
		{
			research.subGroup = NO_RESEARCH_ICON;
		}

		//set key topic
		unsigned int keyTopic = ini.value("keyTopic", 0).toUInt();
		ASSERT(keyTopic <= 1, "Invalid keyTopic for research topic - '%s' ", getStatsName(&research));
		if (keyTopic <= 1)
		{
			research.keyTopic = ini.value("keyTopic", 0).toUInt();
		}
		else
		{
			research.keyTopic = 0;
		}

		//special flag to not reveal research from "give all" and not to research with "research all" cheats
		unsigned int excludeFromCheats = ini.value("excludeFromCheats", 0).toUInt();
		ASSERT(excludeFromCheats <= 1, "Invalid excludeFromCheats for research topic - '%s' ", getStatsName(&research));
		if (excludeFromCheats <= 1)
		{
			research.excludeFromCheats = ini.value("excludeFromCheats", 0).toUInt();
		}
		else
		{
			research.excludeFromCheats = 0;
		}

		//set tech code
		UBYTE techCode = ini.value("techCode", 0).toUInt();
		ASSERT(techCode <= 1, "Invalid tech code for research topic - '%s' ", getStatsName(&research));
		if (techCode == 0)
		{
			research.techCode = TC_MAJOR;
		}
		else
		{
			research.techCode = TC_MINOR;
		}

		//get flags when to disable tech
		UBYTE disabledWhen = ini.value("disabledWhen", 0).toUInt();
		ASSERT(disabledWhen <= MPFLAGS_MAX, "Invalid disabled tech flag for research topic - '%s' ", getStatsName(&research));
		research.disabledWhen = disabledWhen;

		//set the iconID
		WzString iconID = ini.value("iconID", "").toWzString();
		if (iconID.compare("") != 0)
		{
			research.iconID = setIconID(iconID.toUtf8().c_str(), getStatsName(&research));
		}
		else
		{
			research.iconID = NO_RESEARCH_ICON;
		}

		//get the IMDs used in the interface
		WzString statID = ini.value("statID", "").toWzString();
		research.psStat = nullptr;
		if (statID.compare("") != 0)
		{
			//try find the stat with given name
			research.psStat = getBaseStatsFromName(statID);
			ASSERT_OR_RETURN(false, research.psStat, "Could not find stats for %s research %s", statID.toUtf8().c_str(), getStatsName(&research));
		}

		WzString imdName = ini.value("imdName", "").toWzString();
		if (imdName.compare("") != 0)
		{
			research.pIMD = modelGet(imdName);
			ASSERT(research.pIMD != nullptr, "Cannot find the research PIE '%s' for record '%s'", imdName.toUtf8().data(), getStatsName(&research));
		}

		WzString imdName2 = ini.value("imdName2", "").toWzString();
		if (imdName2.compare("") != 0)
		{
			research.pIMD2 = modelGet(imdName2);
			ASSERT(research.pIMD2 != nullptr, "Cannot find the 2nd research '%s' PIE for record '%s'", imdName2.toUtf8().data(), getStatsName(&research));
		}

		WzString msgName = ini.value("msgName", "").toWzString();
		if (msgName.compare("") != 0)
		{
			//check its a major tech code
			ASSERT(research.techCode == TC_MAJOR, "This research should not have a message associated with it, '%s' the message will be ignored!", getStatsName(&research));
			if (research.techCode == TC_MAJOR)
			{
				research.pViewData = getViewData(msgName);
			}
		}

		//set the researchPoints
		unsigned int resPoints = ini.value("researchPoints", 0).toUInt();
		ASSERT_OR_RETURN(false, resPoints <= UWORD_MAX, "Research Points too high for research topic - '%s' ", getStatsName(&research));
		research.researchPoints = resPoints;

		//set the research power
		unsigned int resPower = ini.value("researchPower", 0).toUInt();
		ASSERT_OR_RETURN(false, resPower <= UWORD_MAX, "Research Power too high for research topic - '%s' ", getStatsName(&research));
		research.researchPower = resPower;

		//remember research pre-requisites for futher checking
		preResearch[inc] = ini.value("requiredResearch").toWzStringList();

		//set components results
		std::vector<WzString> compResults = ini.value("resultComponents").toWzStringList();
		for (size_t j = 0; j < compResults.size(); j++)
		{
			WzString compID = compResults[j].trimmed();
			COMPONENT_STATS *pComp = getCompStatsFromName(compID);
			if (pComp != nullptr)
			{
				research.componentResults.push_back(pComp);
			}
			else
			{
				ASSERT(false, "Invalid item '%s' in list of result components of research '%s' ", compID.toUtf8().c_str(), getStatsName(&research));
			}
		}

		//set replaced components
		std::vector<WzString> replacedComp = ini.value("replacedComponents").toWzStringList();
		for (size_t j = 0; j < replacedComp.size(); j++)
		{
			//read pair of components oldComponent:newComponent
			std::vector<WzString> pair = replacedComp[j].split(":");
			ASSERT(pair.size() == 2, "Invalid item '%s' in list of replaced components of research '%s'. Required format: 'oldItem:newItem, item1:item2'", replacedComp[j].toUtf8().c_str(), getStatsName(&research));
			if (pair.size() != 2)
			{
				continue; //skip invalid entries
			}
			WzString oldCompID = pair[0].trimmed();
			WzString newCompID = pair[1].trimmed();
			COMPONENT_STATS *oldComp = getCompStatsFromName(oldCompID);
			if (oldComp == nullptr)
			{
				ASSERT(false, "Invalid item '%s' in list of replaced components of research '%s'. Wrong component code.", oldCompID.toUtf8().c_str(), getStatsName(&research));
				continue;
			}
			COMPONENT_STATS *newComp = getCompStatsFromName(newCompID);
			if (newComp == nullptr)
			{
				ASSERT(false, "Invalid item '%s' in list of replaced components of research '%s'. Wrong component code.", newCompID.toUtf8().c_str(), getStatsName(&research));
				continue;
			}
			RES_COMP_REPLACEMENT replItem;
			replItem.pOldComponent = oldComp;
			replItem.pNewComponent = newComp;
			research.componentReplacement.push_back(replItem);
		}

		//set redundant components
		std::vector<WzString> redComp = ini.value("redComponents").toWzStringList();
		for (size_t j = 0; j < redComp.size(); j++)
		{
			WzString compID = redComp[j].trimmed();
			COMPONENT_STATS *pComp = getCompStatsFromName(compID);
			if (pComp == nullptr)
			{
				ASSERT(false, "Invalid item '%s' in list of redundant components of research '%s' ", compID.toUtf8().c_str(), getStatsName(&research));
			}
			else
			{
				research.pRedArtefacts.push_back(pComp);
			}
		}

		//set result structures
		std::vector<WzString> resStruct = ini.value("resultStructures").toWzStringList();
		for (size_t j = 0; j < resStruct.size(); j++)
		{
			WzString strucID = resStruct[j].trimmed();
			int structIndex = getStructStatFromName(strucID);
			ASSERT(structIndex >= 0, "Invalid item '%s' in list of result structures of research '%s' ", strucID.toUtf8().c_str(), getStatsName(&research));
			if (structIndex >= 0)
			{
				research.pStructureResults.push_back(structIndex);
			}
		}

		//set required structures
		std::vector<WzString> reqStruct = ini.value("requiredStructures").toWzStringList();
		for (size_t j = 0; j < reqStruct.size(); j++)
		{
			WzString strucID = reqStruct[j].trimmed();
			int structIndex = getStructStatFromName(strucID.toUtf8().c_str());
			ASSERT(structIndex >= 0, "Invalid item '%s' in list of required structures of research '%s' ", strucID.toUtf8().c_str(), getStatsName(&research));
			if (structIndex >= 0)
			{
				research.pStructList.push_back(structIndex);
			}
		}

		//set redundant structures
		std::vector<WzString> redStruct = ini.value("redStructures").toWzStringList();
		for (size_t j = 0; j < redStruct.size(); j++)
		{
			WzString strucID = redStruct[j].trimmed();
			int structIndex = getStructStatFromName(strucID.toUtf8().c_str());
			ASSERT(structIndex >= 0, "Invalid item '%s' in list of redundant structures of research '%s' ", strucID.toUtf8().c_str(), getStatsName(&research));
			if (structIndex >= 0)
			{
				research.pRedStructs.push_back(structIndex);
			}
		}

		asResearch.push_back(research);
		ini.endGroup();
	}

	//Load and check research pre-requisites (need do it AFTER loading research items)
	for (size_t inc = 0; inc < asResearch.size(); inc++)
	{
		std::vector<WzString> &preRes = preResearch[inc];
		for (size_t j = 0; j < preRes.size(); j++)
		{
			WzString resID = preRes[j].trimmed();
			RESEARCH *preResItem = getResearch(resID.toUtf8().c_str());
			ASSERT(preResItem != nullptr, "Invalid item '%s' in list of pre-requisites of research '%s' ", resID.toUtf8().c_str(), getStatsName(&asResearch[inc]));
			if (preResItem != nullptr)
			{
				asResearch[inc].pPRList.push_back(preResItem->index);
			}
		}
	}

	if (auto cycle = CycleDetection::detectCycle())
	{
		debug(LOG_ERROR, "A cycle was detected in the research dependency graph:");
		for (auto research: cycle.value())
		{
			debug(LOG_ERROR, "\t-> %s", research->id.toUtf8().c_str());
		}
		return false;
	}

	// populate research category info
	resCategories.clear(); // must clear because we re-process the entire asResearch list if loading more than one research file
	for (size_t inc = 0; inc < asResearch.size(); inc++)
	{
		const auto& cat = asResearch[inc].category;
		if (cat.isEmpty())
		{
			continue;
		}
		resCategories[cat].push_back(inc);
	}
	for (auto& cat : resCategories)
	{
		auto& membersOfCategory = cat.second;
		std::stable_sort(membersOfCategory.begin(), membersOfCategory.end(), [](size_t idxA, size_t idxB) -> bool {
			return isResAPrereqForResB(idxA, idxB);
		});
		uint16_t prog = 1;
		size_t categorySize = membersOfCategory.size();
		for (const auto& inc : membersOfCategory)
		{
			asResearch[inc].categoryProgress = prog;
			asResearch[inc].categoryMax = categorySize;
			prog++;
		}
	}

	// If the first research json file does not explicitly set calculationMode, default to compat
	if (!researchUpgradeCalcMode.has_value())
	{
		researchUpgradeCalcMode = ResearchUpgradeCalculationMode::Compat;
	}

	return true;
}

bool researchAvailable(int inc, UDWORD playerID, QUEUE_MODE mode)
{
	if (playerID >= MAX_PLAYERS)
	{
		return false;
	}

	// Decide whether to use IsResearchCancelledPending/IsResearchStartedPending or IsResearchCancelled/IsResearchStarted.
	bool (*IsResearchCancelledFunc)(PLAYER_RESEARCH const *) = IsResearchCancelledPending;
	bool (*IsResearchStartedFunc)(PLAYER_RESEARCH const *) = IsResearchStartedPending;
	if (mode == ModeImmediate)
	{
		IsResearchCancelledFunc = IsResearchCancelled;
		IsResearchStartedFunc = IsResearchStarted;
	}

	UDWORD				incPR, incS;
	bool				bPRFound, bStructFound;

	// if its a cancelled topic - add to list
	if (IsResearchCancelledFunc(&asPlayerResList[playerID][inc]))
	{
		return true;
	}
	// Ignore disabled
	if (IsResearchDisabled(&asPlayerResList[playerID][inc]))
	{
		return false;
	}
	// if the topic is possible and has not already been researched - add to list
	if ((IsResearchPossible(&asPlayerResList[playerID][inc])))
	{
		if (!IsResearchCompleted(&asPlayerResList[playerID][inc])
		    && !IsResearchStartedFunc(&asPlayerResList[playerID][inc]))
		{
			return true;
		}
	}

	// if single player mode and key topic, then ignore cos can't do it!
	if (!bMultiPlayer && asResearch[inc].keyTopic)
	{
		return false;
	}

	bool researchStarted = IsResearchStartedFunc(&asPlayerResList[playerID][inc]);
	if (researchStarted)
	{
		STRUCTURE *psBuilding = findResearchingFacilityByResearchIndex(playerID, inc);  // May fail to find the structure here, if the research is merely pending, not actually started.
		if (psBuilding != nullptr && psBuilding->status == SS_BEING_BUILT)
		{
			researchStarted = false;  // Although research is started, the facility is currently being upgraded or demolished, so we want to be able to research this elsewhere.
		}
	}

	// make sure that the research is not completed  or started by another researchfac
	if (!IsResearchCompleted(&asPlayerResList[playerID][inc]) && !researchStarted)
	{
		// Research is not completed  ... also  it has not been started by another researchfac

		// if there aren't any PR's - go to next topic
		if (asResearch[inc].pPRList.empty())
		{
			return false;
		}

		// check for pre-requisites
		bPRFound = true;
		for (incPR = 0; incPR < asResearch[inc].pPRList.size(); incPR++)
		{
			if (IsResearchCompleted(&(asPlayerResList[playerID][asResearch[inc].pPRList[incPR]])) == 0)
			{
				// if haven't pre-requisite - quit checking rest
				bPRFound = false;
				break;
			}
		}
		if (!bPRFound)
		{
			// if haven't pre-requisites, skip the rest of the checks
			return false;
		}

		// check for structure effects
		bStructFound = true;
		for (incS = 0; incS < asResearch[inc].pStructList.size(); incS++)
		{
			if (!checkSpecificStructExists(asResearch[inc].pStructList[incS], playerID))
			{
				//if not built, quit checking
				bStructFound = false;
				break;
			}
		}
		if (!bStructFound)
		{
			// if haven't all structs built, skip to next topic
			return false;
		}
		return true;
	}
	return false;
}

/*
Function to check what can be researched for a particular player at any one
instant.

A topic can be researched if the playerRes 'possible' flag has been set (by script)
or if the research pre-req topics have been researched. A check is made for any
structures that are required to have been built for topics that do not have
the 'possible' flag set.

 **NB** A topic with zero PR's can ONLY be researched once the 'possible' flag
 has been set.

There can only be 'limit' number of entries
'topic' is the currently researched topic
*/
// NOTE by AJL may 99 - skirmish now has it's own version of this, skTopicAvail.
std::vector<uint16_t> fillResearchList(UDWORD playerID, nonstd::optional<UWORD> topic, UWORD limit)
{
	std::vector<uint16_t> list;

	for (auto inc = 0; inc < asResearch.size(); inc++)
	{
		// if the inc matches the 'topic' - automatically add to the list
		if ((topic.has_value() && inc == topic.value()) || researchAvailable(inc, playerID, ModeQueue))
		{
			list.push_back(inc);
			if (list.size() == limit)
			{
				return list;
			}
		}
	}

	return list;
}

class internal_execution_context_base : public wzapi::execution_context_base
{
public:
	virtual ~internal_execution_context_base() { }
public:
	virtual void throwError(const char *expr, int line, const char *function) const override
	{
		// do nothing, since the error was already logged and we're not actually running a script
	}
};

static inline int64_t iDivCeil(int64_t dividend, int64_t divisor)
{
	ASSERT_OR_RETURN(0, divisor != 0, "Divide by 0");
	bool hasPosQuotient = (dividend >= 0) == (divisor >= 0);
	// C++11 defines the behavior of % to be truncated
	return (dividend / divisor) + static_cast<int64_t>((dividend % divisor != 0 && hasPosQuotient));
}

static inline int64_t iDivFloor(int64_t dividend, int64_t divisor)
{
	ASSERT_OR_RETURN(0, divisor != 0, "Divide by 0");
	bool hasNegQuotient = (dividend >= 0) != (divisor >= 0);
	// C++11 defines the behavior of % to be truncated
	return (dividend / divisor) - static_cast<int64_t>((dividend % divisor != 0 && hasNegQuotient));
}

static void eventResearchedHandleUpgrades(const RESEARCH *psResearch, const STRUCTURE *psStruct, int player)
{
	if (cachedStatsObject.is_null()) { cachedStatsObject = wzapi::constructStatsObject(); }
	if (cachedPerPlayerUpgrades.empty()) { cachedPerPlayerUpgrades = wzapi::getUpgradesObject(); }
	internal_execution_context_base temp_no_throw_context;

	debug(LOG_RESEARCH, "RESEARCH : %s(%s) for %d", psResearch->name.toUtf8().c_str(), psResearch->id.toUtf8().c_str(), player);

	ASSERT_OR_RETURN(, player >= 0 && player < cachedPerPlayerUpgrades.size(), "Player %d does not exist in per-player upgrades?", player);
	auto &playerRawUpgradeChangeTotals = cachedPerPlayerRawUpgradeChange[player];
	const auto upgradeCalcMode = getResearchUpgradeCalcMode();

	PlayerUpgradeCounts tempStats;

	// iterate over all research results
	for (size_t i = 0; i < psResearch->results.size(); i++)
	{
		auto& v = psResearch->results[i];
		// Required members of research upgrades: "class", "parameter", "value"
#define RS_GET_REQUIRED_RESULT_PROPERTY(resultVar, name, typecheckFuncName) \
	auto resultVar = v.find(name); \
	if (resultVar == v.end()) \
	{ \
		ASSERT(false, "Research(\"%s\").results[%zu]: Missing required parameter: \"%s\"", psResearch->id.toUtf8().c_str(), i, name); \
		continue; \
	} \
	if (!resultVar->typecheckFuncName()) \
	{ \
		ASSERT(false, "Research(\"%s\").results[%zu][\"%s\"]: Unexpected value type: \"%s\"", psResearch->id.toUtf8().c_str(), i, name, resultVar->type_name()); \
		continue; \
	}
		RS_GET_REQUIRED_RESULT_PROPERTY(it_ctype, "class", is_string)
		RS_GET_REQUIRED_RESULT_PROPERTY(it_parameter, "parameter", is_string)
		RS_GET_REQUIRED_RESULT_PROPERTY(it_value, "value", is_number_integer)
		std::string ctype = it_ctype->get<std::string>();
		std::string parameter = it_parameter->get<std::string>();
		int64_t value = it_value->get<int64_t>();
		auto it_filterparam = v.find("filterParameter"); // optional
		auto it_filtervalue = v.find("filterValue"); // required if "filterParameter" is specified
		if (it_filterparam != v.end())
		{
			if (!it_filterparam->is_string())
			{
				ASSERT(false, "Research(\"%s\").results[%zu][\"%s\"]: Unexpected value type: \"%s\"", psResearch->id.toUtf8().c_str(), i, "filterParameter", it_parameter->type_name());
				continue;
			}
			if (it_filtervalue == v.end())
			{
				// ERROR: Supplied a filterParameter but not a filterValue
				ASSERT(false, "Research(\"%s\").results[%zu]: Missing \"%s\" property (required when \"filterParameter\" is specified)", psResearch->id.toUtf8().c_str(), i, "filterParameter");
				continue;
			}
		}
		debug(LOG_RESEARCH, "    RESULT : class=\"%s\" parameter=\"%s\" value=%" PRIi64 " filter=\"%s\" filterval=%s", ctype.c_str(), parameter.c_str(), value, (it_filterparam != v.end()) ? it_filterparam->get<std::string>().c_str() : "", (it_filtervalue != v.end()) ? it_filtervalue->dump().c_str() : "");

		auto pPlayerEntityClass = cachedPerPlayerUpgrades[player].find(ctype);
		if (!pPlayerEntityClass)
		{
			ASSERT(pPlayerEntityClass, "Unknown entity class: %s", ctype.c_str());
			continue;
		}
		bool isBodyClass = ctype == "Body";
		bool isWeaponClass = ctype == "Weapon";
		for (auto cname : *pPlayerEntityClass) // iterate over all components of this type
		{
			const auto statsEntityClassObj = cachedStatsObject.find(ctype);
			if (statsEntityClassObj == cachedStatsObject.end())
			{
				ASSERT(false, "Parameter \"%s\" does not exist in Stats[%s][%s] ?", parameter.c_str(), ctype.c_str(), cname.first.c_str());
				continue;
			}
			const auto statsEntityObj = statsEntityClassObj->find(cname.first);
			if (statsEntityObj == statsEntityClassObj->end())
			{
				ASSERT(false, "Parameter \"%s\" does not exist in Stats[%s][%s] ?", parameter.c_str(), ctype.c_str(), cname.first.c_str());
				continue;
			}

			if (it_filterparam != v.end())
			{
				// more specific filter
				std::string filterparam = it_filterparam.value().get<std::string>();
				const auto pStatsFilterParameterValue = statsEntityObj->find(filterparam);
				if (pStatsFilterParameterValue == statsEntityObj->end())
				{
					// Did not find filter parameter
					continue;
				}
				if (!((*pStatsFilterParameterValue) == it_filtervalue.value()))
				{
					// Non-matching filter parameter
					continue;
				}
			}

			const auto pStatsParameterValue = statsEntityObj->find(parameter);
			if (pStatsParameterValue == statsEntityObj->end())
			{
				// Did not find it??
				ASSERT(false, "Parameter \"%s\" does not exist in Stats[%s][%s] ?", parameter.c_str(), ctype.c_str(), cname.first.c_str());
				continue;
			}

			if (pStatsParameterValue->is_array()) // (ex. modifying "RankThresholds")
			{
				nlohmann::json dst = cname.second.getPropertyValue(temp_no_throw_context, parameter);
				if (!dst.is_array() || (dst.size() != pStatsParameterValue->size()))
				{
					// The Upgrades parameter unexpectedly is not an array, or not the same array size
					ASSERT(false, "Upgrades parameter \"%s\" value (type %s) does not match Stats[%s][%s] value type (%s) or size (%zu)", parameter.c_str(), dst.type_name(), ctype.c_str(), cname.first.c_str(), pStatsParameterValue->type_name(), pStatsParameterValue->size());
					continue;
				}
				for (size_t x = 0; x < dst.size(); x++)
				{
					const auto& statsOriginalValueX = pStatsParameterValue->at(x);
					if (!statsOriginalValueX.is_number_integer())
					{
						ASSERT(false, "Unexpected parameter \"%s[%zu]\" value type (%s) in Stats[%s][%s]", parameter.c_str(), x, pStatsParameterValue->type_name(), ctype.c_str(), cname.first.c_str());
						continue;
					}
					const auto& currentUpgradesValue_json = dst[x];
					if (!currentUpgradesValue_json.is_number_integer())
					{
						// The Upgrades parameter unexpectedly is not an integer
						ASSERT(false, "Upgrades parameter \"%s[%zu]\" value type (%s) does not match Stats[%s][%s] value type (%s)", parameter.c_str(), x, currentUpgradesValue_json.type_name(), ctype.c_str(), cname.first.c_str(), pStatsParameterValue->type_name());
						continue;
					}
					int64_t currentUpgradesValue = currentUpgradesValue_json.get<int64_t>();
					int64_t scaledChange = (statsOriginalValueX.get<int64_t>() * value);
					int64_t newUpgradesChange = (value < 0) ? iDivFloor(scaledChange, 100) : iDivCeil(scaledChange, 100);
					int64_t newUpgradesValue = (currentUpgradesValue + newUpgradesChange);
					if (currentUpgradesValue_json.is_number_unsigned())
					{
						// original was unsigned integer - round anything less than 0 up to 0
						newUpgradesValue = std::max<int64_t>(newUpgradesValue, 0);
						dst[x] = static_cast<uint64_t>(newUpgradesValue);
					}
					else
					{
						dst[x] = newUpgradesValue;
					}
				}
				cname.second.setPropertyValue(temp_no_throw_context, parameter, dst);
				debug(LOG_RESEARCH, "    upgraded to : %s", dst.dump().c_str());
			}
			else if (pStatsParameterValue->is_number_integer())
			{
				const int64_t statsOriginalValue = pStatsParameterValue->get<int64_t>();
				if (statsOriginalValue <= 0) // only applies if stat has above zero value already
				{
					continue;
				}
				nlohmann::json currentUpgradesValue_json = cname.second.getPropertyValue(temp_no_throw_context, parameter);
				if (!currentUpgradesValue_json.is_number_integer())
				{
					// The Upgrades parameter unexpectedly is not an integer
					ASSERT(false, "Upgrades parameter \"%s\" value type (%s) does not match Stats[%s][%s] value type (%s)", parameter.c_str(), currentUpgradesValue_json.type_name(), ctype.c_str(), cname.first.c_str(), pStatsParameterValue->type_name());
					continue;
				}
				int64_t currentUpgradesValue = currentUpgradesValue_json.get<int64_t>();
				int64_t scaledChange = (statsOriginalValue * value);
				int64_t newUpgradesChange = 0;
				int64_t newUpgradesValue = 0;
				switch (upgradeCalcMode)
				{
					case ResearchUpgradeCalculationMode::Compat:
						// Default / compat cumulative upgrade handling (the only option for many years - from at least 3.x/(3.2+?)-4.4.2?)
						// This can accumulate noticeable error, especially if repeatedly upgrading small values by small percentages (commonly impacted: armour, thermal)
						// However, research.json created and tested during this long period may be expecting this outcome / behavior
						newUpgradesChange = (value < 0) ? iDivFloor(scaledChange, 100) : iDivCeil(scaledChange, 100);
						newUpgradesValue = (currentUpgradesValue + newUpgradesChange);
						break;
					case ResearchUpgradeCalculationMode::Improved:
					{
						// "Improved" cumulative upgrade handling (significantly reduces accumulated errors)
						auto& compUpgradeTotals = playerRawUpgradeChangeTotals[cname.first];
						auto& cumulativeUpgradeScaledChange = compUpgradeTotals[parameter];
						cumulativeUpgradeScaledChange += scaledChange;
						newUpgradesValue = statsOriginalValue + ((cumulativeUpgradeScaledChange < 0) ? iDivFloor(cumulativeUpgradeScaledChange, 100) : iDivCeil(cumulativeUpgradeScaledChange, 100));
						newUpgradesChange = newUpgradesValue - currentUpgradesValue;
						break;
					}
				}
				if (currentUpgradesValue_json.is_number_unsigned())
				{
					// original was unsigned integer - round anything less than 0 up to 0
					newUpgradesValue = std::max<int64_t>(newUpgradesValue, 0);
					cname.second.setPropertyValue(temp_no_throw_context, parameter, static_cast<uint64_t>(newUpgradesValue));
				}
				else
				{
					cname.second.setPropertyValue(temp_no_throw_context, parameter, newUpgradesValue);
				}
				debug(LOG_RESEARCH, "      upgraded \"%s\" to %" PRIi64 " by %" PRIi64 "", cname.first.c_str(), newUpgradesValue, newUpgradesChange);
				if (isWeaponClass)
				{
					auto impactClass = statsEntityObj->find("ImpactClass");
					if (impactClass != statsEntityObj->end())
					{
						tempStats.numWeaponImpactClassUpgrades[impactClass->get<std::string>()]++;
					}
					else
					{
						ASSERT(false, "Did not find expected \"ImpactClass\" member in Stats[%s][%s]", ctype.c_str(), cname.first.c_str());
					}
				}
				else if (isBodyClass && parameter == "Armour")
				{
					auto bodyClass = statsEntityObj->find("BodyClass");
					if (bodyClass != statsEntityObj->end())
					{
						tempStats.numBodyClassArmourUpgrades[bodyClass->get<std::string>()]++;
					}
					else
					{
						ASSERT(false, "Did not find expected \"BodyClass\" member in Stats[%s][%s]", ctype.c_str(), cname.first.c_str());
					}
				}
				else if (isBodyClass && parameter == "Thermal")
				{
					auto bodyClass = statsEntityObj->find("BodyClass");
					if (bodyClass != statsEntityObj->end())
					{
						tempStats.numBodyClassThermalUpgrades[bodyClass->get<std::string>()]++;
					}
					else
					{
						ASSERT(false, "Did not find expected \"BodyClass\" member in Stats[%s][%s]", ctype.c_str(), cname.first.c_str());
					}
				}
			}
			else
			{
				// unexpected type
				// Research stats / upgrades are not supposed to expose non-integer types, as the core stats / game state calculations must use integer arithmetic
				ASSERT(false, "Unexpected parameter \"%s\" value type (%s) in Stats[%s][%s]", parameter.c_str(), pStatsParameterValue->type_name(), ctype.c_str(), cname.first.c_str());
				continue;
			}
		}
	}

	// accumulate stats
	for (auto& bodyClassUpgrades : tempStats.numBodyClassArmourUpgrades)
	{
		const auto& bodyClass = bodyClassUpgrades.first;
		if (bodyClassUpgrades.second > 0)
		{
			playerUpgradeCounts[player].numBodyClassArmourUpgrades[bodyClass]++;
			debug(LOG_RESEARCH, "  Player[%d], Armour[%s] grade: %" PRIu32 "", player, bodyClass.c_str(), playerUpgradeCounts[player].numBodyClassArmourUpgrades[bodyClass]);
		}
	}
	for (auto& bodyClassUpgrades : tempStats.numBodyClassThermalUpgrades)
	{
		const auto& bodyClass = bodyClassUpgrades.first;
		if (bodyClassUpgrades.second > 0)
		{
			playerUpgradeCounts[player].numBodyClassThermalUpgrades[bodyClass]++;
			debug(LOG_RESEARCH, "  Player[%d], Thermal[%s] grade: %" PRIu32 "", player, bodyClass.c_str(), playerUpgradeCounts[player].numBodyClassThermalUpgrades[bodyClass]);
		}
	}
	for (auto& weaponUpgrades : tempStats.numWeaponImpactClassUpgrades)
	{
		const auto& impactClass = weaponUpgrades.first;
		if (weaponUpgrades.second > 0)
		{
			playerUpgradeCounts[player].numWeaponImpactClassUpgrades[impactClass]++;
			debug(LOG_RESEARCH, "  Player[%d], Weapon[%s] grade: %" PRIu32 "", player, impactClass.c_str(), playerUpgradeCounts[player].numWeaponImpactClassUpgrades[impactClass]);
		}
	}
}

static void makeComponentRedundant(UBYTE &state)
{
	switch (state)
	{
	case AVAILABLE:
		state = REDUNDANT;
		break;
	case UNAVAILABLE:
		state = REDUNDANT_UNAVAILABLE;
		break;
	case FOUND:
		state = REDUNDANT_FOUND;
		break;
	}
}

static void makeComponentAvailable(UBYTE &state)
{
	switch (state)
	{
	case UNAVAILABLE:
	case FOUND:
		state = AVAILABLE;
		break;
	case REDUNDANT_UNAVAILABLE:
	case REDUNDANT_FOUND:
		state = REDUNDANT;
		break;
	}
}

/* process the results of a completed research topic */
void researchResult(UDWORD researchIndex, UBYTE player, bool bDisplay, STRUCTURE *psResearchFacility, bool bTrigger)
{
	ASSERT_OR_RETURN(, researchIndex < asResearch.size(), "Invalid research index %u", researchIndex);
	ASSERT_OR_RETURN(, player < MAX_PLAYERS, "invalid player: %" PRIu8 "", player);

	RESEARCH                    *pResearch = &asResearch[researchIndex];
	MESSAGE						*pMessage;
	//the message gets sent to console
	char						consoleMsg[MAX_RESEARCH_MSG_SIZE];

	syncDebug("researchResult(%u, %u, …)", researchIndex, player);

	MakeResearchCompleted(&asPlayerResList[player][researchIndex]);

	//check for structures to be made available
	for (unsigned short pStructureResult : pResearch->pStructureResults)
	{
		makeComponentAvailable(apStructTypeLists[player][pStructureResult]);
	}

	//check for structures to be made redundant
	for (unsigned short pRedStruct : pResearch->pRedStructs)
	{
		makeComponentRedundant(apStructTypeLists[player][pRedStruct]);
	}

	//check for component replacement
	if (!pResearch->componentReplacement.empty())
	{
		for (auto &ri : pResearch->componentReplacement)
		{
			COMPONENT_STATS *pOldComp = ri.pOldComponent;
			replaceComponent(ri.pNewComponent, pOldComp, player);
			makeComponentRedundant(apCompLists[player][pOldComp->compType][pOldComp->index]);
		}
	}

	//check for artefacts to be made available
	for (auto &componentResult : pResearch->componentResults)
	{
		//determine the type of artefact
		COMPONENT_TYPE type = componentResult->compType;
		//set the component state to AVAILABLE
		int compInc = componentResult->index;
		makeComponentAvailable(apCompLists[player][type][compInc]);
		//check for default sensor
		if (type == COMP_SENSOR && asSensorStats[compInc].location == LOC_DEFAULT)
		{
			aDefaultSensor[player] = compInc;
		}
		//check for default ECM
		else if (type == COMP_ECM && asECMStats[compInc].location == LOC_DEFAULT)
		{
			aDefaultECM[player] = compInc;
		}
		//check for default Repair
		else if (type == COMP_REPAIRUNIT && asRepairStats[compInc].location == LOC_DEFAULT)
		{
			aDefaultRepair[player] = compInc;
			enableSelfRepair(player);
		}
	}

	//check for artefacts to be made redundant
	for (auto &pRedArtefact : pResearch->pRedArtefacts)
	{
		COMPONENT_TYPE type = pRedArtefact->compType;
		makeComponentRedundant(apCompLists[player][type][pRedArtefact->index]);
	}

	//Add message to player's list if Major Topic
	if ((pResearch->techCode == TC_MAJOR) && bDisplay)
	{
		//only play sound if major topic
		if (player == selectedPlayer)
		{
			audio_QueueTrack(ID_SOUND_MAJOR_RESEARCH);
		}

		//check there is viewdata for the research topic - just don't add message if not!
		if (pResearch->pViewData != nullptr)
		{
			pMessage = addMessage(MSG_RESEARCH, false, player);
			if (pMessage != nullptr)
			{
				pMessage->pViewData = pResearch->pViewData;
				jsDebugMessageUpdate();
			}
		}
	}
	else if (player == selectedPlayer && bDisplay)
	{
		audio_QueueTrack(ID_SOUND_RESEARCH_COMPLETED);
	}

	if (player == selectedPlayer && bDisplay)
	{
		//add console text message
		snprintf(consoleMsg, MAX_RESEARCH_MSG_SIZE, _("Research completed: %s"), getLocalizedStatsName(pResearch));
		addConsoleMessage(consoleMsg, LEFT_JUSTIFY, SYSTEM_MESSAGE);
	}

	if (psResearchFacility)
	{
		psResearchFacility->pFunctionality->researchFacility.psSubject = nullptr;		// Make sure topic is cleared
	}

	eventResearchedHandleUpgrades(pResearch, psResearchFacility, player);

	triggerEventResearched(pResearch, psResearchFacility, player);
}

/*This function is called when the research files are reloaded*/
bool ResearchShutDown()
{
	ResearchRelease();
	return true;
}

/*This function is called when a game finishes*/
void ResearchRelease()
{
	asResearch.clear();
	researchUpgradeCalcMode = nullopt;
	resCategories.clear();
	for (auto &i : asPlayerResList)
	{
		i.clear();
	}
	cachedStatsObject = nlohmann::json(nullptr);
	cachedPerPlayerUpgrades.clear();
	for (auto &p : cachedPerPlayerRawUpgradeChange)
	{
		p.clear();
	}
	playerUpgradeCounts = std::vector<PlayerUpgradeCounts>(MAX_PLAYERS);
}

/*puts research facility on hold*/
void holdResearch(STRUCTURE *psBuilding, QUEUE_MODE mode)
{
	ASSERT_OR_RETURN(, psBuilding->pStructureType->type == REF_RESEARCH, "structure not a research facility");

	RESEARCH_FACILITY *psResFac = &psBuilding->pFunctionality->researchFacility;

	if (mode == ModeQueue)
	{
		sendStructureInfo(psBuilding, STRUCTUREINFO_HOLDRESEARCH, nullptr);
		setStatusPendingHold(*psResFac);
		return;
	}

	if (psResFac->psSubject)
	{
		//set the time the research facility was put on hold
		psResFac->timeStartHold = gameTime;
		//play audio to indicate on hold
		if (psBuilding->player == selectedPlayer)
		{
			audio_PlayTrack(ID_SOUND_WINDOWCLOSE);
		}
	}

	delPowerRequest(psBuilding);
}

/*release a research facility from hold*/
void releaseResearch(STRUCTURE *psBuilding, QUEUE_MODE mode)
{
	ASSERT_OR_RETURN(, psBuilding->pStructureType->type == REF_RESEARCH, "structure not a research facility");

	RESEARCH_FACILITY *psResFac = &psBuilding->pFunctionality->researchFacility;

	if (mode == ModeQueue)
	{
		sendStructureInfo(psBuilding, STRUCTUREINFO_RELEASERESEARCH, nullptr);
		setStatusPendingRelease(*psResFac);
		return;
	}

	if (psResFac->psSubject && psResFac->timeStartHold)
	{
		//adjust the start time for the current subject
		psResFac->timeStartHold = 0;
	}
}


/*

	Cancel All Research for player 0

*/
void CancelAllResearch(UDWORD pl)
{
	if (pl >= MAX_PLAYERS) { return; }

	for (STRUCTURE* psCurr : apsStructLists[pl])
	{
		if (psCurr->pStructureType->type == REF_RESEARCH)
		{
			if (
			    (((RESEARCH_FACILITY *)psCurr->pFunctionality) != nullptr)
			    && (((RESEARCH_FACILITY *)psCurr->pFunctionality)->psSubject != nullptr)
			)
			{
				debug(LOG_NEVER, "canceling research for %p\n", static_cast<void *>(psCurr));
				cancelResearch(psCurr, ModeQueue);
			}
		}

	}
}

/** Sets the status of the topic to cancelled and stores the current research points accquired */
void cancelResearch(STRUCTURE *psBuilding, QUEUE_MODE mode)
{
	UDWORD              topicInc;
	PLAYER_RESEARCH	    *pPlayerRes;

	ASSERT_OR_RETURN(, psBuilding->pStructureType && psBuilding->pStructureType->type == REF_RESEARCH, "Structure not a research facility");

	RESEARCH_FACILITY *psResFac = &psBuilding->pFunctionality->researchFacility;
	if (!(RESEARCH *)psResFac->psSubject)
	{
		debug(LOG_SYNC, "Invalid research topic");
		return;
	}
	topicInc = ((RESEARCH *)psResFac->psSubject)->index;
	ASSERT_OR_RETURN(, topicInc <= asResearch.size(), "Invalid research topic %u (max %d)", topicInc, (int)asResearch.size());
	pPlayerRes = &asPlayerResList[psBuilding->player][topicInc];
	if (psBuilding->pStructureType->type == REF_RESEARCH)
	{
		if (mode == ModeQueue)
		{
			// Tell others that we want to stop researching something.
			sendResearchStatus(psBuilding, topicInc, psBuilding->player, false);
			// Immediately tell the UI that we can research this now. (But don't change the game state.)
			MakeResearchCancelledPending(pPlayerRes);
			setStatusPendingCancel(*psResFac);
			return;  // Wait for our message before doing anything. (Whatever this function does...)
		}

		//check if waiting to accrue power
		if (pPlayerRes->currentPoints == 0)
		{
			// Reset this topic as not having been researched
			ResetResearchStatus(pPlayerRes);
		}
		else
		{
			// Set the researched flag
			MakeResearchCancelled(pPlayerRes);
		}

		// Initialise the research facility's subject
		psResFac->psSubject = nullptr;

		delPowerRequest(psBuilding);
	}
}

/* For a given view data get the research this is related to */
RESEARCH *getResearchForMsg(const VIEWDATA *pViewData)
{
	for (auto &inc : asResearch)
	{
		if (inc.pViewData == pViewData)	// compare the pointer
		{
			return &inc;
		}
	}
	return nullptr;
}

//set the iconID based on the name read in in the stats
static UWORD setIconID(const char *pIconName, const char *pName)
{
	//compare the names with those created in 'Framer'
	if (!strcmp(pIconName, "IMAGE_ROCKET"))
	{
		return IMAGE_ROCKET;
	}
	if (!strcmp(pIconName, "IMAGE_CANNON"))
	{
		return IMAGE_CANNON;
	}
	if (!strcmp(pIconName, "IMAGE_HOVERCRAFT"))
	{
		return IMAGE_HOVERCRAFT;
	}
	if (!strcmp(pIconName, "IMAGE_ECM"))
	{
		return IMAGE_ECM;
	}
	if (!strcmp(pIconName, "IMAGE_PLASCRETE"))
	{
		return IMAGE_PLASCRETE;
	}
	if (!strcmp(pIconName, "IMAGE_TRACKS"))
	{
		return IMAGE_TRACKS;
	}

	if (!strcmp(pIconName, "IMAGE_RES_DROIDTECH"))
	{
		return IMAGE_RES_DROIDTECH;
	}

	if (!strcmp(pIconName, "IMAGE_RES_WEAPONTECH"))
	{
		return IMAGE_RES_WEAPONTECH;
	}

	if (!strcmp(pIconName, "IMAGE_RES_COMPUTERTECH"))
	{
		return IMAGE_RES_COMPUTERTECH;
	}

	if (!strcmp(pIconName, "IMAGE_RES_POWERTECH"))
	{
		return IMAGE_RES_POWERTECH;
	}

	if (!strcmp(pIconName, "IMAGE_RES_SYSTEMTECH"))
	{
		return IMAGE_RES_SYSTEMTECH;
	}

	if (!strcmp(pIconName, "IMAGE_RES_STRUCTURETECH"))
	{
		return IMAGE_RES_STRUCTURETECH;
	}

	if (!strcmp(pIconName, "IMAGE_RES_CYBORGTECH"))
	{
		return IMAGE_RES_CYBORGTECH;
	}

	if (!strcmp(pIconName, "IMAGE_RES_DEFENCE"))
	{
		return IMAGE_RES_DEFENCE;
	}

	if (!strcmp(pIconName, "IMAGE_RES_QUESTIONMARK"))
	{
		return IMAGE_RES_QUESTIONMARK;
	}

	if (!strcmp(pIconName, "IMAGE_RES_GRPACC"))
	{
		return IMAGE_RES_GRPACC;
	}

	if (!strcmp(pIconName, "IMAGE_RES_GRPUPG"))
	{
		return IMAGE_RES_GRPUPG;
	}

	if (!strcmp(pIconName, "IMAGE_RES_GRPREP"))
	{
		return IMAGE_RES_GRPREP;
	}

	if (!strcmp(pIconName, "IMAGE_RES_GRPROF"))
	{
		return IMAGE_RES_GRPROF;
	}

	if (!strcmp(pIconName, "IMAGE_RES_GRPDAM"))
	{
		return IMAGE_RES_GRPDAM;
	}

	// Add more names as images are created
	ASSERT(false, "Invalid icon graphic %s for topic %s", pIconName, pName);

	return NO_RESEARCH_ICON;	// Should never get here.
}

SDWORD	mapIconToRID(UDWORD iconID)
{
	switch (iconID)
	{
	case IMAGE_ROCKET:
		return (RID_ROCKET);
		break;
	case IMAGE_CANNON:
		return (RID_CANNON);
		break;
	case IMAGE_HOVERCRAFT:
		return (RID_HOVERCRAFT);
		break;
	case IMAGE_ECM:
		return (RID_ECM);
		break;
	case IMAGE_PLASCRETE:
		return (RID_PLASCRETE);
		break;
	case IMAGE_TRACKS:
		return (RID_TRACKS);
		break;
	case IMAGE_RES_DROIDTECH:
		return (RID_DROIDTECH);
		break;
	case IMAGE_RES_WEAPONTECH:
		return (RID_WEAPONTECH);
		break;
	case IMAGE_RES_COMPUTERTECH:
		return (RID_COMPUTERTECH);
		break;
	case IMAGE_RES_POWERTECH:
		return (RID_POWERTECH);
		break;
	case IMAGE_RES_SYSTEMTECH:
		return (RID_SYSTEMTECH);
		break;
	case IMAGE_RES_STRUCTURETECH:
		return (RID_STRUCTURETECH);
		break;
	case IMAGE_RES_CYBORGTECH:
		return (RID_CYBORGTECH);
		break;
	case IMAGE_RES_DEFENCE:
		return (RID_DEFENCE);
		break;
	case IMAGE_RES_QUESTIONMARK:
		return (RID_QUESTIONMARK);
		break;
	case IMAGE_RES_GRPACC:
		return (RID_GRPACC);
		break;
	case IMAGE_RES_GRPUPG:
		return (RID_GRPUPG);
		break;
	case IMAGE_RES_GRPREP:
		return (RID_GRPREP);
		break;
	case IMAGE_RES_GRPROF:
		return (RID_GRPROF);
		break;
	case IMAGE_RES_GRPDAM:
		return (RID_GRPDAM);
		break;
	default:
		return (-1); //pass back a value that can never have been set up
		break;
	}
}

//return a pointer to a research topic based on the name
RESEARCH *getResearch(const char *pName)
{
	for (auto &inc : asResearch)
	{
		if (inc.id.compare(pName) == 0)
		{
			return &inc;
		}
	}
	debug(LOG_WARNING, "Unknown research - %s", pName);
	return nullptr;
}

/* looks through the players lists of structures and droids to see if any are using
 the old component - if any then replaces them with the new component */
static void replaceComponent(COMPONENT_STATS *pNewComponent, COMPONENT_STATS *pOldComponent,
                             UBYTE player)
{
	ASSERT_OR_RETURN(, player < MAX_PLAYERS, "invalid player: %" PRIu8 "", player);

	COMPONENT_TYPE oldType = pOldComponent->compType;
	int oldCompInc = pOldComponent->index;
	COMPONENT_TYPE newType = pNewComponent->compType;
	int newCompInc = pNewComponent->index;

	//check old and new type are the same
	if (oldType != newType)
	{
		return;
	}

	replaceDroidComponent(apsDroidLists[player], oldType, oldCompInc, newCompInc);
	replaceDroidComponent(mission.apsDroidLists[player], oldType, oldCompInc, newCompInc);
	replaceDroidComponent(apsLimboDroids[player], oldType, oldCompInc, newCompInc);
	const auto replaceComponentInTemplate = [oldType, oldCompInc, newCompInc](DROID_TEMPLATE* psTemplates) {
		switch (oldType)
		{
		case COMP_BODY:
		case COMP_BRAIN:
		case COMP_PROPULSION:
		case COMP_REPAIRUNIT:
		case COMP_ECM:
		case COMP_SENSOR:
		case COMP_CONSTRUCT:
			if (psTemplates->asParts[oldType] == (SDWORD)oldCompInc)
			{
				psTemplates->asParts[oldType] = newCompInc;
			}
			break;
		case COMP_WEAPON:
			for (int inc = 0; inc < psTemplates->numWeaps; inc++)
			{
				if (psTemplates->asWeaps[inc] == oldCompInc)
				{
					psTemplates->asWeaps[inc] = newCompInc;
				}
			}
			break;
		default:
			//unknown comp type
			debug(LOG_ERROR, "Unknown component type - invalid Template");
			return true;
		}
		return true;
	};
	//check thru the templates
	enumerateTemplates(player, replaceComponentInTemplate);
	// also check build queues
	for (STRUCTURE *psCBuilding : apsStructLists[player])
	{
		if ((psCBuilding->pStructureType->type == STRUCTURE_TYPE::REF_FACTORY ||
			psCBuilding->pStructureType->type == STRUCTURE_TYPE::REF_CYBORG_FACTORY ||
			psCBuilding->pStructureType->type == STRUCTURE_TYPE::REF_VTOL_FACTORY) &&
			psCBuilding->pFunctionality->factory.psSubject != nullptr)
		{
			replaceComponentInTemplate(psCBuilding->pFunctionality->factory.psSubject);
		}
	}
	replaceStructureComponent(apsStructLists[player], oldType, oldCompInc, newCompInc, player);
	replaceStructureComponent(mission.apsStructLists[player], oldType, oldCompInc, newCompInc, player);
}

/*Looks through all the currently allocated stats to check the name is not
a duplicate*/
static bool checkResearchName(RESEARCH *psResearch, UDWORD numStats)
{
	for (size_t inc = 0; inc < numStats; inc++)
	{

		ASSERT_OR_RETURN(false, asResearch[inc].id.compare(psResearch->id) != 0,
		                 "Research name has already been used - %s", getStatsName(psResearch));
	}
	return true;
}

/* Sets the 'possible' flag for a player's research so the topic will appear in
the research list next time the Research Facility is selected */
bool enableResearch(RESEARCH *psResearch, UDWORD player)
{
	UDWORD				inc;

	ASSERT_OR_RETURN(false, psResearch, "No such research topic");
	ASSERT_OR_RETURN(false, player < MAX_PLAYERS, "invalid player: %" PRIu32 "", player);

	inc = psResearch->index;
	if (inc > asResearch.size())
	{
		ASSERT(false, "enableResearch: Invalid research topic - %s", getStatsName(psResearch));
		return false;
	}

	int prevState = intGetResearchState();

	//found, so set the flag
	MakeResearchPossible(&asPlayerResList[player][inc]);

	if (player == selectedPlayer)
	{
		//set the research reticule button to flash if research facility is free
		intNotifyResearchButton(prevState);
	}

	return true;
}

/*find the last research topic of importance that the losing player did and
'give' the results to the reward player*/
void researchReward(UBYTE losingPlayer, UBYTE rewardPlayer)
{
	UDWORD topicIndex = 0, researchPoints = 0, rewardID = 0;

	//look through the losing players structures to find a research facility
	for (const STRUCTURE *psStruct : apsStructLists[losingPlayer])
	{
		if (psStruct->pStructureType->type == REF_RESEARCH)
		{
			RESEARCH_FACILITY *psFacility = (RESEARCH_FACILITY *)psStruct->pFunctionality;
			if (psFacility->psBestTopic)
			{
				topicIndex = ((RESEARCH *)psFacility->psBestTopic)->ref - STAT_RESEARCH;
				if (topicIndex && !IsResearchCompleted(&asPlayerResList[rewardPlayer][topicIndex]))
				{
					//if it cost more - it is better (or should be)
					if (researchPoints < asResearch[topicIndex].researchPoints)
					{
						//store the 'best' topic
						researchPoints = asResearch[topicIndex].researchPoints;
						rewardID = topicIndex;
					}
				}
			}
		}
	}

	//if a topic was found give the reward player the results of that research
	if (rewardID)
	{
		researchResult(rewardID, rewardPlayer, true, nullptr, true);
		if (rewardPlayer == selectedPlayer)
		{
			//name the actual reward
			CONPRINTF("%s :- %s",
			                          _("Research Award"),
			                          getLocalizedStatsName(&asResearch[rewardID]));
		}
	}
}

/*flag self repair so droids can start when idle*/
void enableSelfRepair(UBYTE player)
{
	ASSERT_OR_RETURN(, player < MAX_PLAYERS, "invalid player: %" PRIu8 "", player);
	bSelfRepair[player] = true;
}

/*check to see if any research has been completed that enables self repair*/
bool selfRepairEnabled(UBYTE player)
{
	ASSERT_OR_RETURN(false, player < MAX_PLAYERS, "invalid player: %" PRIu8 "", player);
	if (bSelfRepair[player])
	{
		return true;
	}
	else
	{
		return false;
	}
}

/*for a given list of droids, replace the old component if exists*/
void replaceDroidComponent(DroidList& pList, UDWORD oldType, UDWORD oldCompInc,
                           UDWORD newCompInc)
{
	//check thru the droids
	for (DROID* psDroid : pList)
	{
		switchComponent(psDroid, oldType, oldCompInc, newCompInc);
		// Need to replace the units inside the transporter
		if (psDroid->isTransporter())
		{
			replaceTransDroidComponents(psDroid, oldType, oldCompInc, newCompInc);
		}
	}
}

/*replaces any components necessary for units that are inside a transporter*/
void replaceTransDroidComponents(DROID *psTransporter, UDWORD oldType,
                                 UDWORD oldCompInc, UDWORD newCompInc)
{
	ASSERT(psTransporter->isTransporter(), "invalid unit type");

	for (DROID* psCurr : psTransporter->psGroup->psList)
	{
		if (psCurr != psTransporter)
		{
			switchComponent(psCurr, oldType, oldCompInc, newCompInc);
		}
	}
}

void replaceStructureComponent(StructureList& pList, UDWORD oldType, UDWORD oldCompInc,
                               UDWORD newCompInc, UBYTE player)
{
	int			inc;

	// If the type is not one we are interested in, then don't bother checking
	if (!(oldType == COMP_ECM || oldType == COMP_SENSOR || oldType == COMP_WEAPON))
	{
		return;
	}

	//check thru the structures
	for (STRUCTURE* psStructure : pList)
	{
		switch (oldType)
		{
		case COMP_WEAPON:
			for (inc = 0; inc < psStructure->numWeaps; inc++)
			{
				if (psStructure->asWeaps[inc].nStat > 0)
				{
					if (psStructure->asWeaps[inc].nStat == oldCompInc)
					{
						psStructure->asWeaps[inc].nStat = newCompInc;
					}
				}
			}
			break;
		default:
			//ignore all other component types
			break;
		}
	}
}

/*swaps the old component for the new one for a specific droid*/
static void switchComponent(DROID *psDroid, UDWORD oldType, UDWORD oldCompInc,
                            UDWORD newCompInc)
{
	ASSERT_OR_RETURN(, psDroid != nullptr, "Invalid droid pointer");

	switch (oldType)
	{
	case COMP_BODY:
	case COMP_BRAIN:
	case COMP_PROPULSION:
	case COMP_REPAIRUNIT:
	case COMP_ECM:
	case COMP_SENSOR:
	case COMP_CONSTRUCT:
		if (psDroid->asBits[oldType] == oldCompInc)
		{
			psDroid->asBits[oldType] = (UBYTE)newCompInc;
		}
		break;
	case COMP_WEAPON:
		// Can only be one weapon now
		if (psDroid->asWeaps[0].nStat > 0)
		{
			if (psDroid->asWeaps[0].nStat == oldCompInc)
			{
				psDroid->asWeaps[0].nStat = newCompInc;
			}
		}
		break;
	default:
		//unknown comp type
		debug(LOG_ERROR, "Unknown component type - invalid droid");
		abort();
		return;
	}
}

static inline bool allyResearchSortFunction(AllyResearch const &a, AllyResearch const &b)
{
	if (a.active         != b.active)
	{
		return a.active;
	}
	if (a.timeToResearch != b.timeToResearch)
	{
		return (unsigned)a.timeToResearch < (unsigned)b.timeToResearch;    // Unsigned cast = sort -1 as infinite.
	}
	if (a.powerNeeded    != b.powerNeeded)
	{
		return (unsigned)a.powerNeeded    < (unsigned)b.powerNeeded;
	}
	if (a.completion     != b.completion)
	{
		return           a.completion     >           b.completion;
	}
	return           a.player         <           b.player;
}

std::vector<AllyResearch> const &listAllyResearch(unsigned ref)
{
	static uint32_t lastGameTime = ~0;
	static std::map<unsigned, std::vector<AllyResearch>> researches;
	static const std::vector<AllyResearch> noAllyResearch;

	if (selectedPlayer >= MAX_PLAYERS)
	{
		return noAllyResearch;
	}

	if (gameTime != lastGameTime)
	{
		lastGameTime = gameTime;
		researches.clear();

		for (int player = 0; player < MAX_PLAYERS; ++player)
		{
			if (player == selectedPlayer || !aiCheckAlliances(selectedPlayer, player) || !alliancesSharedResearch(game.alliance))
			{
				continue;  // Skip this player, not an ally.
			}

			// Check each research facility to see if they are doing this topic. (As opposed to having started the topic, but stopped researching it.)
			for (const STRUCTURE *psStruct : apsStructLists[player])
			{
				RESEARCH_FACILITY *res = (RESEARCH_FACILITY *)psStruct->pFunctionality;
				if (psStruct->pStructureType->type != REF_RESEARCH || res->psSubject == nullptr)
				{
					continue;  // Not a researching research facility.
				}

				RESEARCH const &subject = *res->psSubject;
				PLAYER_RESEARCH const &playerRes = asPlayerResList[player][subject.index];
				unsigned cRef = subject.ref;

				AllyResearch r;
				r.player = player;
				r.completion = playerRes.currentPoints;
				r.powerNeeded = checkPowerRequest(psStruct);
				r.timeToResearch = -1;
				if (r.powerNeeded == -1)
				{
					r.timeToResearch = (subject.researchPoints - playerRes.currentPoints) / std::max(getBuildingResearchPoints(psStruct), 1);
				}
				r.active = psStruct->status == SS_BUILT;
				researches[cRef].push_back(r);
			}
		}
		for (auto &research : researches)
		{
			std::sort(research.second.begin(), research.second.end(), allyResearchSortFunction);
		}
	}

	std::map<unsigned, std::vector<AllyResearch>>::const_iterator i = researches.find(ref);
	if (i == researches.end())
	{
		return noAllyResearch;
	}
	return i->second;
}

/* Recursively disable research for all players */
static void RecursivelyDisableResearchByID(size_t index)
{
	if (IsResearchDisabled(&asPlayerResList[0][index]))
	{
		return;
	}

	for (int player = 0; player < MAX_PLAYERS; ++player)
	{
		DisableResearch(&asPlayerResList[player][index]);
	}

	for (size_t inc = 0; inc < asResearch.size(); inc++)
	{
		for (size_t prereq = 0; prereq < asResearch[inc].pPRList.size(); prereq++)
		{
			if (asResearch[inc].pPRList[prereq] == index)
			{
				RecursivelyDisableResearchByID(inc);
			}
		}
	}
}

void RecursivelyDisableResearchByFlags(UBYTE flags)
{
	for (size_t inc = 0; inc < asResearch.size(); inc++)
	{
		if (asResearch[inc].disabledWhen & flags)
		{
			RecursivelyDisableResearchByID(inc);
		}
	}
}
