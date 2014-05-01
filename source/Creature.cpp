/*
 *  Copyright (C) 2011-2014  OpenDungeons Team
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*TODO list:
 * - replace hardcoded calculations by scripts and/or read the numbers from XML defintion files
 * - the doTurn() functions needs script support
 */

#include "Creature.h"

#include "CreatureAction.h"
#include "BattleField.h"
#include "Weapon.h"
#include "GameMap.h"
#include "RenderRequest.h"
#include "SoundEffectsHelper.h"
#include "CreatureSound.h"
#include "Player.h"
#include "Seat.h"
#include "RenderManager.h"
#include "Random.h"
#include "LogManager.h"
#include "CullingQuad.h"
#include "Helper.h"

#include <CEGUI/System.h>
#include <CEGUI/WindowManager.h>
#include <CEGUI/Window.h>
#include <CEGUI/UDim.h>
#include <CEGUI/Vector.h>

#include <OgreQuaternion.h>
#include <OgreVector3.h>
#include <OgreVector2.h>

#include <cmath>
#include <algorithm>

#if OGRE_PLATFORM == OGRE_PLATFORM_WIN32
#define snprintf_is_banned_in_OD_code _snprintf
#endif

static const int MAX_LEVEL = 30;
//TODO: make this read from definition file?
static const int MaxGoldCarriedByWorkers = 1500;

Creature::Creature(GameMap* gameMap, const std::string& name) :
    mTracingCullingQuad      (NULL),
    mWeaponL                 (NULL),
    mWeaponR                 (NULL),
    mHomeTile                (NULL),
    mDefinition              (NULL),
    mIsOnMap                 (false),
    mHasVisualDebuggingEntities (false),
    mAwakeness               (100.0),
    mMaxHP                   (100.0),
    mMaxMana                 (100.0),
    mLevel                   (1),
    mHp                      (100.0),
    mMana                    (100.0),
    mExp                     (0.0),
    mDigRate                 (1.0),
    mDanceRate               (1.0),
    mDeathCounter            (10),
    mGold                    (0),
    mBattleFieldAgeCounter   (0),
    mTrainWait               (0),
    mPreviousPositionTile    (NULL),
    mBattleField             (new BattleField()),
    mTrainingDojo            (NULL),
    mStatsWindow             (NULL),
    mSound                   (SoundEffectsHelper::getSingleton().createCreatureSound(getName()))
{
    setGameMap(gameMap);

    setName(name);

    setIsOnMap(false);

    setObjectType(GameEntity::creature);

    pushAction(CreatureAction::idle);
}

/* Destructor is needed when removing from Quadtree */
Creature::~Creature()
{
    mTracingCullingQuad->entry->creature_list.remove(this);
    mTracingCullingQuad->mortuaryInsert(this);

    if(mBattleField != NULL)
        delete mBattleField;

    // Delete weapons
    if (mWeaponL)
        delete mWeaponL;
    if (mWeaponR)
        delete mWeaponR;
}

//! \brief A function which returns a string describing the IO format of the << and >> operators.
std::string Creature::getFormat()
{
    //NOTE:  When this format changes changes to RoomPortal::spawnCreature() may be necessary.
    return "className\tname\tposX\tposY\tposZ\tcolor\tweaponL"
        + Weapon::getFormat() + "\tweaponR" + Weapon::getFormat() + "\tHP\tmana";
}

//! \brief A matched function to transport creatures between files and over the network.
std::ostream& operator<<(std::ostream& os, Creature *c)
{
    assert(c);

    // Check creature weapons
    Weapon* wL = c->mWeaponL;
    if (wL == NULL)
        wL = new Weapon("none", 0.0, 1.0, 0.0, "L", c);
    Weapon* wR = c->mWeaponR;
    if (wR == NULL)
        wR = new Weapon("none", 0.0, 1.0, 0.0, "R", c);

    os << c->mDefinition->getClassName() << "\t" << c->getName() << "\t";

    os << c->getPosition().x << "\t";
    os << c->getPosition().y << "\t";
    os << c->getPosition().z << "\t";
    os << c->getColor() << "\t";
    os << wL << "\t" << wR << "\t";
    os << c->getHP() << "\t";
    os << c->getMana() << "\t";
    os << c->getLevel();

    // If we had to create dummy weapons for serialization, delete them now.
    if (c->mWeaponL == NULL)
        delete wL;
    if (c->mWeaponR == NULL)
        delete wR;

    return os;
}

/*! \brief A matched function to transport creatures between files and over the network.
 *
 */
std::istream& operator>>(std::istream& is, Creature *c)
{
    double xLocation = 0.0, yLocation = 0.0, zLocation = 0.0;
    double tempDouble = 0.0;
    std::string className;
    std::string tempString;

    is >> className;
    is >> tempString;

    if (tempString.compare("autoname") == 0)
        tempString = c->getUniqueCreatureName();

    c->setName(tempString);

    is >> xLocation >> yLocation >> zLocation;
    c->setPosition(Ogre::Vector3((Ogre::Real)xLocation, (Ogre::Real)yLocation, (Ogre::Real)zLocation));

    int color = 0;
    is >> color;
    c->setColor(color);

    // TODO: Load weapon from a catalog file.
    c->setWeaponL(new Weapon(std::string(), 0.0, 0.0, 0.0, std::string()));
    is >> c->mWeaponL;

    c->setWeaponR(new Weapon(std::string(), 0.0, 0.0, 0.0, std::string()));
    is >> c->mWeaponR;

    is >> tempDouble;
    c->setHP(tempDouble);
    is >> tempDouble;
    c->setMana(tempDouble);
    is >> tempDouble;
    c->setLevel(tempDouble);

    // Copy the class based items
    CreatureDefinition *creatureClass = c->getGameMap()->getClassDescription(className);
    if (creatureClass != NULL)
    {
        c->setCreatureDefinition(creatureClass);
    }
    assert(c->mDefinition);

    return is;
}

void Creature::loadFromLine(const std::string& line, Creature* c)
{
    assert(c);

    std::vector<std::string> elems = Helper::split(line, '\t');

    std::string creatureName = elems[1];
    if (creatureName.compare("autoname") == 0)
        creatureName = c->getUniqueCreatureName();
    c->setName(creatureName);

    double xLocation = Helper::toDouble(elems[2]);
    double yLocation = Helper::toDouble(elems[3]);
    double zLocation = Helper::toDouble(elems[4]);

    c->setPosition(Ogre::Vector3((Ogre::Real)xLocation, (Ogre::Real)yLocation, (Ogre::Real)zLocation));

    c->setColor(Helper::toInt(elems[5]));

    // TODO: Load weapons from a catalog file.
    c->setWeaponL(new Weapon(elems[6], Helper::toDouble(elems[7]),
                             Helper::toDouble(elems[8]), Helper::toDouble(elems[9]), "L", c));

    c->setWeaponR(new Weapon(elems[10], Helper::toDouble(elems[11]),
                             Helper::toDouble(elems[12]), Helper::toDouble(elems[13]), "R", c));

    c->setHP(Helper::toDouble(elems[14]));
    c->setMana(Helper::toDouble(elems[15]));
    c->setLevel(Helper::toDouble(elems[16]));

    // Copy the class based items
    CreatureDefinition *creatureClass = c->getGameMap()->getClassDescription(elems[0]);
    if (creatureClass != 0)
    {
        c->mDefinition = creatureClass;
    }
    assert(c->mDefinition);
}

/*! \brief Changes the creature's position to a new position.
 *
 *  This is an overloaded function which just calls Creature::setPosition(double x, double y, double z).
 */
void Creature::setPosition(const Ogre::Vector3& v)
{
    // If we are on the gameMap we may need to update the tile we are in
    if (getIsOnMap())
    {
        // We are on the map
        // Move the creature relative to its parent scene node.  We record the
        // tile the creature is in before and after the move to properly
        // maintain the results returned by the positionTile() function.
        Tile *oldPositionTile = positionTile();

        MovableGameEntity::setPosition(v);
        Tile *newPositionTile = positionTile();

        if (oldPositionTile != newPositionTile)
        {
            if (oldPositionTile != 0)
                oldPositionTile->removeCreature(this);

            if (positionTile() != 0)
                positionTile()->addCreature(this);
        }

        mTracingCullingQuad->moveEntryDelta(this,get2dPosition());
    }
    else
    {
        // We are not on the map
        MovableGameEntity::setPosition(v);
    }

    // Create a RenderRequest to notify the render queue that the scene node for this creature needs to be moved.
    RenderRequest *request = new RenderRequest;
    request->type = RenderRequest::moveSceneNode;
    request->str = getName() + "_node";
    request->vec = v;

    // Add the request to the queue of rendering operations to be performed before the next frame.
    RenderManager::queueRenderRequest(request);
}

void Creature::setHP(double nHP)
{
    mHp = nHP;

    updateStatsWindow();
}

double Creature::getHP() const
{
    double tempDouble = mHp;
    return tempDouble;
}

void Creature::setMana(double nMana)
{
    mMana = nMana;

    updateStatsWindow();
}

double Creature::getMana() const
{
    double tempDouble = mMana;

    return tempDouble;
}

void Creature::setIsOnMap(bool nIsOnMap)
{
    mIsOnMap = nIsOnMap;
}

bool Creature::getIsOnMap() const
{
    bool tempBool = mIsOnMap;

    return tempBool;
}

void Creature::setWeaponL(Weapon* wL)
{
    if (mWeaponL)
        delete mWeaponL;
    mWeaponL = wL;
    if (!mWeaponL)
        return;

    mWeaponL->setParentCreature(this);
    mWeaponL->setHandString("L");
}

void Creature::setWeaponR(Weapon* wR)
{
    if (mWeaponR)
        delete mWeaponR;
    mWeaponR = wR;
    if (!mWeaponR)
        return;

    mWeaponR->setParentCreature(this);
    mWeaponR->setHandString("R");
}

void Creature::attach()
{
    RenderRequest *request = new RenderRequest;
    request->type = RenderRequest::attachCreature;
    request->p = this;

    // Add the request to the queue of rendering operations to be performed before the next frame.
    RenderManager::queueRenderRequest(request);
}


void Creature::detach()
{
    RenderRequest *request = new RenderRequest;
    request->type = RenderRequest::detachCreature;
    request->p = this;

    // Add the request to the queue of rendering operations to be performed before the next frame.
    RenderManager::queueRenderRequest(request);
}


/*! \brief The main AI routine which decides what the creature will do and carries out that action.
 *
 * The doTurn routine is the heart of the Creature AI subsystem.  The other,
 * higher level, functions such as GameMap::doTurn() ultimately just call this
 * function to make the creatures act.
 *
 * The function begins in a pre-cognition phase which prepares the creature's
 * brain state for decision making.  This involves generating lists of known
 * about creatures, either through sight, hearing, keeper knowledge, etc, as
 * well as some other bookkeeping stuff.
 *
 * Next the function enters the cognition phase where the creature's current
 * state is examined and a decision is made about what to do.  The state of the
 * creature is in the form of a queue, which is really used more like a stack.
 * At the beginning of the game the 'idle' action is pushed onto each
 * creature's actionQueue, this action is never removed from the tail end of
 * the queue and acts as a "last resort" for when the creature completely runs
 * out of things to do.  Other actions such as 'walkToTile' or 'attackObject'
 * are then pushed onto the front of the queue and will determine the
 * creature's future behavior.  When actions are complete they are popped off
 * the front of the action queue, causing the creature to revert back into the
 * state it was in when the actions was placed onto the queue.  This allows
 * actions to be carried out recursively, i.e. if a creature is trying to dig a
 * tile and it is not nearby it can begin walking toward the tile as a new
 * action, and when it arrives at the tile it will revert to the 'digTile'
 * action.
 *
 * In the future there should also be a post-cognition phase to do any
 * additional checks after it tries to move, etc.
 */
void Creature::doTurn()
{
    // If we are not standing somewhere on the map, do nothing.
    if (positionTile() == NULL)
        return;

    // Check to see if we have earned enough experience to level up.
    while (mExp >= 5 * (getLevel() + std::pow(getLevel() / 3.0, 2)) && getLevel() < 100)
        doLevelUp();

    // Heal.
    mHp += 0.1;
    if (mHp > getMaxHp())
        mHp = getMaxHp();

    // Regenerate mana.
    mMana += 0.45;
    if (mMana > mMaxMana)
        mMana = mMaxMana;

    mAwakeness -= 0.15;

    // Look at the surrounding area
    updateVisibleTiles();
    mVisibleEnemyObjects         = getVisibleEnemyObjects();
    mReachableEnemyObjects       = getReachableAttackableObjects(mVisibleEnemyObjects, 0, 0);
    mEnemyObjectsInRange         = getEnemyObjectsInRange(mVisibleEnemyObjects);
    mLivingEnemyObjectsInRange   = GameEntity::removeDeadObjects(mEnemyObjectsInRange);
    mVisibleAlliedObjects        = getVisibleAlliedObjects();
    mReachableAlliedObjects      = getReachableAttackableObjects(mVisibleAlliedObjects, 0, 0);

    std::vector<Tile*> markedTiles;

    if (mDefinition->getDigRate() > 0.0)
        markedTiles = getVisibleMarkedTiles();

    decideNextAction();

    // The loopback variable allows creatures to begin processing a new
    // action immediately after some other action happens.
    bool loopBack = false;
    unsigned int loops = 0;

    do
    {
        ++loops;
        loopBack = false;

        // Carry out the current task
        if (!mActionQueue.empty())
        {
            CreatureAction topActionItem = mActionQueue.front();

            switch (topActionItem.getType())
            {
                case CreatureAction::idle:
                    loopBack = handleIdleAction();
                    break;

                case CreatureAction::walkToTile:
                    loopBack = handleWalkToTileAction();
                    break;

                case CreatureAction::claimTile:
                    loopBack = handleClaimTileAction();
                    break;

                case CreatureAction::claimWallTile:
                    loopBack = handleClaimWallTileAction();
                    break;

                case CreatureAction::digTile:
                    loopBack = handleDigTileAction();
                    break;

                case CreatureAction::depositGold:
                    loopBack = handleDepositGoldAction();
                    break;

                case CreatureAction::findHome:
                    loopBack = handleFindHomeAction();
                    break;

                case CreatureAction::sleep:
                    loopBack = handleSleepAction();
                    break;

                case CreatureAction::train:
                    loopBack = handleTrainingAction();
                    break;

                case CreatureAction::attackObject:
                    loopBack = handleAttackAction();
                    break;

                case CreatureAction::maneuver:
                    loopBack = handleManeuverAction();
                    break;

                default:
                    LogManager::getSingleton().logMessage("ERROR:  Unhandled action type in Creature::doTurn().");
                    popAction();
                    loopBack = false;
                    break;
            }
        }
        else
        {
            LogManager::getSingleton().logMessage("ERROR:  Creature has empty action queue in doTurn(), this should not happen.");
            loopBack = false;
        }
    } while (loopBack && loops < 20);

    if(loops >= 20)
    {
        LogManager::getSingleton().logMessage("> 20 loops in Creature::doTurn name:" + getName() +
                " colour: " + Ogre::StringConverter::toString(getColor()) + ". Breaking out..");
    }

    // Update the visual debugging entities
    //if we are standing in a different tile than we were last turn
    if (mHasVisualDebuggingEntities && positionTile() != mPreviousPositionTile)
    {
        //TODO: This destroy and re-create is kind of a hack as its likely only a few
        //tiles will actually change.
        destroyVisualDebugEntities();
        createVisualDebugEntities();
    }
}

void Creature::decideNextAction()
{
    // If the creature can see enemies that are reachable.
    if (!mReachableEnemyObjects.empty())
    {
        // Check to see if there is any combat actions (maneuvering/attacking) in our action queue.
        bool alreadyFighting = false;
        for (unsigned int i = 0, size = mActionQueue.size(); i < size; ++i)
        {
            if (mActionQueue[i].getType() == CreatureAction::attackObject
                    || mActionQueue[i].getType() == CreatureAction::maneuver)
            {
                alreadyFighting = true;
                break;
            }
        }

        // If we are not already fighting with a creature or maneuvering then start doing so.
        if (!alreadyFighting)
        {
            if (Random::Double(0.0, 1.0) < (mDefinition->isWorker() ? 0.05 : 0.8))
            {
                mBattleFieldAgeCounter = 0;
                pushAction(CreatureAction::maneuver);
                // Jump immediately to the action processor since we don't want to decide to
                //train or something if there are enemies around.
                return;
            }
        }
    }

    if (mBattleFieldAgeCounter > 0)
        --mBattleFieldAgeCounter;

    if (mDefinition->isWorker())
        return;

    // Check whether the creature is weak
    bool isWeak = (mHp < mMaxHP / 3);

    // Check to see if we have found a "home" tile where we can sleep yet.
    if (isWeak || (Random::Double(0.0, 1.0) < 0.03 && mHomeTile == NULL
        && peekAction().getType() != CreatureAction::findHome))
    {
        // Check to see if there are any quarters owned by our color that we can reach.
        std::vector<Room*> tempRooms = getGameMap()->getRoomsByTypeAndColor(Room::quarters, getColor());
        tempRooms = getGameMap()->getReachableRooms(tempRooms, positionTile(), mDefinition->getTilePassability());
        if (!tempRooms.empty())
        {
            pushAction(CreatureAction::findHome);
            return;
        }
    }

    // If we have found a home tile to sleep on, see if we are tired enough to want to go to sleep.
    if (isWeak || (mHomeTile != NULL && 100.0 * std::pow(Random::Double(0.0, 0.8), 2) > mAwakeness
        && peekAction().getType() != CreatureAction::sleep))
    {
        pushAction(CreatureAction::sleep);
    }
    else if (Random::Double(0.0, 1.0) < 0.1 && Random::Double(0.5, 1.0) < mAwakeness / 100.0
             && peekAction().getType() != CreatureAction::train)
    {
        // Check to see if there is a Dojo we can train at.
        //TODO: Check here to see if the controlling seat has any dojo's to train at, if not then don't try to train.
        pushAction(CreatureAction::train);
        mTrainWait = 0;
    }
}

bool Creature::handleIdleAction()
{
    double diceRoll = Random::Double(0.0, 1.0);
    bool loopBack = false;

    setAnimationState("Idle");

    // Decide to check for diggable tiles
    if (mDefinition->getDigRate() > 0.0 && !getVisibleMarkedTiles().empty())
    {
        loopBack = true;
        pushAction(CreatureAction::digTile);
    }
    // Decide to check for claimable tiles
    else if (mDefinition->getDanceRate() > 0.0 && diceRoll < 0.9)
    {
        loopBack = true;
        pushAction(CreatureAction::claimTile);
    }
    // Decide to deposit the gold we are carrying into a treasury.
    else if (mDefinition->getDigRate() > 0.0 && mGold > 0)
    {
        //TODO: We need a flag to see if we have tried to do this
        // so the creature won't get confused if we are out of space.
        loopBack = true;
        pushAction(CreatureAction::depositGold);
    }

    // Any creature.

    // Decide whether to "wander" a short distance
    if (diceRoll >= 0.6)
        return loopBack;

    // Note: Always return true from now on.
    pushAction(CreatureAction::walkToTile);

    // Workers should move around randomly at large jumps.  Non-workers either wander short distances or follow workers.
    int tempX = 0;
    int tempY = 0;

    if (!mDefinition->isWorker())
    {
        // Non-workers only.

        // Check to see if we want to try to follow a worker around or if we want to try to explore.
        double r = Random::Double(0.0, 1.0);
        //if(creatureJob == weakFighter) r -= 0.2;
        if (r < 0.7)
        {
            bool workerFound = false;
            // Try to find a worker to follow around.
            for (unsigned int i = 0; !workerFound && i < mReachableAlliedObjects.size(); ++i)
            {
                // Check to see if we found a worker.
                if (mReachableAlliedObjects[i]->getObjectType() == GameEntity::creature
                    && static_cast<Creature*>(mReachableAlliedObjects[i])->mDefinition->isWorker())
                {
                    // We found a worker so find a tile near the worker to walk to.  See if the worker is digging.
                    Tile* tempTile = mReachableAlliedObjects[i]->getCoveredTiles()[0];
                    if (static_cast<Creature*>(mReachableAlliedObjects[i])->peekAction().getType()
                            == CreatureAction::digTile)
                    {
                        // Worker is digging, get near it since it could expose enemies.
                        tempX = (int)(static_cast<double>(tempTile->x) + 3.0
                                * Random::gaussianRandomDouble());
                        tempY = (int)(static_cast<double>(tempTile->y) + 3.0
                                * Random::gaussianRandomDouble());
                    }
                    else
                    {
                        // Worker is not digging, wander a bit farther around the worker.
                        tempX = (int)(static_cast<double>(tempTile->x) + 8.0
                                * Random::gaussianRandomDouble());
                        tempY = (int)(static_cast<double>(tempTile->y) + 8.0
                                * Random::gaussianRandomDouble());
                    }
                    workerFound = true;
                }

                // If there are no workers around, choose tiles far away to "roam" the dungeon.
                if (!workerFound)
                {
                    if (!mVisibleTiles.empty())
                    {
                        Tile* tempTile = mVisibleTiles[static_cast<unsigned int>(Random::Double(0.6, 0.8)
                                                                                 * (mVisibleTiles.size() - 1))];
                        tempX = tempTile->x;
                        tempY = tempTile->y;
                    }
                }
            }
        }
        else
        {
            // Randomly choose a tile near where we are standing to walk to.
            if (!mVisibleTiles.empty())
            {
                unsigned int tileIndex = static_cast<unsigned int>(mVisibleTiles.size()
                                                                   * Random::Double(0.1, 0.3));
                Tile* myTile = positionTile();
                std::list<Tile*> tempPath = getGameMap()->path(myTile,
                        mVisibleTiles[tileIndex],
                        mDefinition->getTilePassability());
                if (setWalkPath(tempPath, 2, false))
                {
                    setAnimationState("Walk");
                    pushAction(CreatureAction::walkToTile);
                    return true;
                }
            }
        }
    }
    else
    {
        // Workers only.

        // Choose a tile far away from our current position to wander to.
        if (!mVisibleTiles.empty())
        {
            Tile* tempTile = mVisibleTiles[Random::Uint(mVisibleTiles.size() / 2, mVisibleTiles.size() - 1)];
            tempX = tempTile->x;
            tempY = tempTile->y;
        }
    }

    Tile *tempPositionTile = positionTile();
    std::list<Tile*> result;
    if (tempPositionTile != NULL)
    {
        result = getGameMap()->path(tempPositionTile->x, tempPositionTile->y,
                                    tempX, tempY, mDefinition->getTilePassability());
    }

    getGameMap()->cutCorners(result, mDefinition->getTilePassability());
    if (setWalkPath(result, 2, false))
    {
        setAnimationState("Walk");
        pushAction(CreatureAction::walkToTile);
    }
    return true;
}

bool Creature::handleWalkToTileAction()
{
    //TODO: This should be decided based on some aggressiveness parameter.
    if (Random::Double(0.0, 1.0) < 0.6 && !mEnemyObjectsInRange.empty())
    {
        popAction();
        pushAction(CreatureAction::attackObject);
        clearDestinations();
        return true;
    }

    //TODO: Peek at the item that caused us to walk
    // If we are walking toward a tile we are trying to dig out, check to see if it is still marked for digging.
    bool toDigTile = (mActionQueue[1].getType() == CreatureAction::digTile);
    if (toDigTile)
    {
        Player* tempPlayer = getControllingPlayer();

        // Check to see if the tile is still marked for digging
        unsigned int index = mWalkQueue.size();
        Tile *currentTile = NULL;
        if (index > 0)
            currentTile = getGameMap()->getTile((int) mWalkQueue[index - 1].x,
                    (int) mWalkQueue[index - 1].y);

        if (currentTile != NULL)
        {
            // If it is not marked
            if (tempPlayer != 0 && !currentTile->getMarkedForDigging(tempPlayer))
            {
                // Clear the walk queue
                clearDestinations();
            }
        }
    }

    //cout << "walkToTile ";
    if (mWalkQueue.empty())
    {
        popAction();

        // This extra post is included here because if the break statement happens
        // the one at the end of the 'if' block will not happen.
        return true;
    }
    return false;
}

bool Creature::handleClaimTileAction()
{
    Tile* myTile = positionTile();
    //NOTE:  This is a workaround for the problem with the positionTile() function,
    // it can be removed when that issue is resolved.
    if (myTile == NULL)
    {
        popAction();
        return false;
    }

    // Randomly decide to stop claiming with a small probability
    std::vector<Tile*> markedTiles = getVisibleMarkedTiles();
    if (Random::Double(0.0, 1.0) < 0.1 + 0.2 * markedTiles.size())
    {
        // If there are any visible tiles marked for digging start working on that.
        if (!markedTiles.empty())
        {
            popAction();
            pushAction(CreatureAction::digTile);
            return true;
        }
    }

    // See if the tile we are standing on can be claimed
    if ((myTile->getColor() != getColor() || myTile->colorDouble < 1.0) && myTile->isGroundClaimable())
    {
        //cout << "\nTrying to claim the tile I am standing on.";
        // Check to see if one of the tile's neighbors is claimed for our color
        std::vector<Tile*> neighbors = myTile->getAllNeighbors();
        for (unsigned int j = 0; j < neighbors.size(); ++j)
        {
            // Check to see if the current neighbor is already claimed
            Tile* tempTile = neighbors[j];
            if (tempTile->getColor() == getColor() && tempTile->colorDouble >= 1.0)
            {
                //cout << "\t\tFound a neighbor that is claimed.";
                // If we found a neighbor that is claimed for our side than we can start
                // dancing on this tile.  If there is "left over" claiming that can be done
                // it will spill over into neighboring tiles until it is gone.
                setAnimationState("Claim");
                myTile->claimForColor(getColor(), mDefinition->getDanceRate());
                recieveExp(1.5 * (mDefinition->getDanceRate() / (0.35 + 0.05 * getLevel())));

                // Since we danced on a tile we are done for this turn
                return false;
            }
        }
    }

    // The tile we are standing on is already claimed or is not currently
    // claimable, find candidates for claiming.
    // Start by checking the neighbor tiles of the one we are already in
    std::vector<Tile*> neighbors = myTile->getAllNeighbors();
    while (!neighbors.empty())
    {
        // If the current neighbor is claimable, walk into it and skip to the end of this turn
        int tempInt = Random::Uint(0, neighbors.size() - 1);
        Tile* tempTile = neighbors[tempInt];
        //NOTE:  I don't think the "colorDouble" check should happen here.
        if (tempTile != NULL && tempTile->getTilePassability() == Tile::walkableTile
            && (tempTile->getColor() != getColor() || tempTile->colorDouble < 1.0)
            && tempTile->isGroundClaimable())
        {
            // The neighbor tile is a potential candidate for claiming, to be an actual candidate
            // though it must have a neighbor of its own that is already claimed for our side.
            Tile* tempTile2;
            std::vector<Tile*> neighbors2 = tempTile->getAllNeighbors();
            for (unsigned int i = 0; i < neighbors2.size(); ++i)
            {
                tempTile2 = neighbors2[i];
                if (tempTile2->getColor() == getColor()
                        && tempTile2->colorDouble >= 1.0)
                {
                    clearDestinations();
                    addDestination((Ogre::Real)tempTile->x, (Ogre::Real)tempTile->y);
                    setAnimationState("Walk");
                    return false;
                }
            }
        }

        neighbors.erase(neighbors.begin() + tempInt);
    }

    //cout << "\nLooking at the visible tiles to see if I can claim a tile.";
    // If we still haven't found a tile to claim, check the rest of the visible tiles
    std::vector<Tile*> claimableTiles;
    for (unsigned int i = 0; i < mVisibleTiles.size(); ++i)
    {
        // if this tile is not fully claimed yet or the tile is of another player's color
        Tile* tempTile = mVisibleTiles[i];
        if (tempTile != NULL && tempTile->getTilePassability() == Tile::walkableTile
            && (tempTile->colorDouble < 1.0 || tempTile->getColor() != getColor())
            && tempTile->isGroundClaimable())
        {
            // Check to see if one of the tile's neighbors is claimed for our color
            neighbors = mVisibleTiles[i]->getAllNeighbors();
            for (unsigned int j = 0; j < neighbors.size(); ++j)
            {
                tempTile = neighbors[j];
                if (tempTile->getColor() == getColor()
                        && tempTile->colorDouble >= 1.0)
                {
                    claimableTiles.push_back(tempTile);
                }
            }
        }
    }

    //cout << "  I see " << claimableTiles.size() << " tiles I can claim.";
    // Randomly pick a claimable tile, plot a path to it and walk to it
    unsigned int tempUnsigned = 0;
    Tile* tempTile = NULL;
    while (!claimableTiles.empty())
    {
        // Randomly find a "good" tile to claim.  A good tile is one that has many neighbors
        // already claimed, this makes the claimed are more "round" and less jagged.
        do
        {
            int numNeighborsClaimed = 0;

            // Start by randomly picking a candidate tile.
            tempTile = claimableTiles[Random::Uint(0, claimableTiles.size() - 1)];

            // Count how many of the candidate tile's neighbors are already claimed.
            neighbors = tempTile->getAllNeighbors();
            for (unsigned int i = 0; i < neighbors.size(); ++i)
            {
                if (neighbors[i]->getColor() == getColor() && neighbors[i]->colorDouble >= 1.0)
                    ++numNeighborsClaimed;
            }

            // Pick a random number in [0:1], if this number is high enough, than use this tile to claim.  The
            // bar for success approaches 0 as numTiles approaches N so this will be guaranteed to succeed at,
            // or before the time we get to the last unclaimed tile.  The bar for success is also lowered
            // according to how many neighbors are already claimed.
            //NOTE: The bar can be negative, when this happens we are guarenteed to use this candidate tile.
            double bar = 1.0 - (numNeighborsClaimed / 4.0) - (tempUnsigned / (double) (claimableTiles.size() - 1));
            if (Random::Double(0.0, 1.0) >= bar)
                break;

            // Safety catch to prevent infinite loop in case the bar for success is too high and is never met.
            if (tempUnsigned >= claimableTiles.size() - 1)
                break;

            // Increment the counter indicating how many candidate tiles we have rejected so far.
            ++tempUnsigned;
        } while (true);

        if (tempTile != NULL)
        {
            // If we find a valid path to the tile start walking to it and break
            std::list<Tile*> tempPath = getGameMap()->path(myTile, tempTile, mDefinition->getTilePassability());
            getGameMap()->cutCorners(tempPath, mDefinition->getTilePassability());
            if (setWalkPath(tempPath, 2, false))
            {
                setAnimationState("Walk");
                pushAction(CreatureAction::walkToTile);
                return false;
            }
        }

        // If we got to this point, the tile we randomly picked cannot be gotten to via a
        // valid path.  Delete it from the claimable tiles vector and repeat the outer
        // loop to try to find another valid tile.
        for (unsigned int i = 0; i < claimableTiles.size(); ++i)
        {
            if (claimableTiles[i] == tempTile)
            {
                claimableTiles.erase(claimableTiles.begin() + i);
                break; // Break out of this for loop.
            }
        }
    }

    // We couldn't find a tile to try to claim so we start searching for claimable walls
    popAction();
    pushAction(CreatureAction::claimWallTile);
    return true;
}

bool Creature::handleClaimWallTileAction()
{
    Tile* myTile = positionTile();
    //NOTE:  This is a workaround for the problem with the positionTile() function,
    // it can be removed when that issue is resolved.
    if (myTile == NULL)
    {
        popAction();
        return false;
    }

    // Randomly decide to stop claiming with a small probability
    std::vector<Tile*> markedTiles = getVisibleMarkedTiles();
    if (Random::Double(0.0, 1.0) < 0.1 + 0.2 * markedTiles.size())
    {
        // If there are any visible tiles marked for digging start working on that.
        if (!markedTiles.empty())
        {
            popAction();
            pushAction(CreatureAction::digTile);
            return true;
        }
    }

    //std::cout << "Claim wall" << std::endl;

    // See if any of the tiles is one of our neighbors
    bool wasANeighbor = false;
    std::vector<Tile*> creatureNeighbors = myTile->getAllNeighbors();
    Player* tempPlayer = getControllingPlayer();
    for (unsigned int i = 0; i < creatureNeighbors.size() && !wasANeighbor; ++i)
    {
        if (tempPlayer == NULL)
            break;

        Tile* tempTile = creatureNeighbors[i];

        if (!tempTile->isWallClaimable(getColor()))
            continue;

        // Turn so that we are facing toward the tile we are going to dig out.
        faceToward(tempTile->x, tempTile->y);

        // Dig out the tile by decreasing the tile's fullness.
        setAnimationState("Claim");
        tempTile->claimForColor(getColor(), mDefinition->getDanceRate());
        recieveExp(1.5 * mDefinition->getDanceRate() / 20.0);

        wasANeighbor = true;
        //std::cout << "Claiming wall" << std::endl;
        break;
    }

    // If we successfully found a wall tile to claim then we are done for this turn.
    if (wasANeighbor)
        return false;
    //std::cout << "Looking for a wall to claim" << std::endl;

    // Find paths to all of the neighbor tiles for all of the visible wall tiles.
    std::vector<std::list<Tile*> > possiblePaths;
    std::vector<Tile*> wallTiles = getVisibleClaimableWallTiles();
    for (unsigned int i = 0; i < wallTiles.size(); ++i)
    {
        std::vector<Tile*> neighbors = wallTiles[i]->getAllNeighbors();
        for (unsigned int j = 0; j < neighbors.size(); ++j)
        {
            Tile* neighborTile = neighbors[j];
            if (neighborTile != NULL && neighborTile->getFullness() < 1)
                possiblePaths.push_back(getGameMap()->path(positionTile(), neighborTile, mDefinition->getTilePassability()));
        }
    }

    // Find the shortest path and start walking toward the tile to be dug out
    if (!possiblePaths.empty())
    {
        // Find the N shortest valid paths, see if there are any valid paths shorter than this first guess
        std::vector<std::list<Tile*> > shortPaths;
        for (unsigned int i = 0; i < possiblePaths.size(); ++i)
        {
            // If the current path is long enough to be valid
            unsigned int currentLength = possiblePaths[i].size();
            if (currentLength >= 2)
            {
                shortPaths.push_back(possiblePaths[i]);

                // If we already have enough short paths
                if (shortPaths.size() > 5)
                {
                    unsigned int longestLength, longestIndex;

                    // Kick out the longest
                    longestLength = shortPaths[0].size();
                    longestIndex = 0;
                    for (unsigned int j = 1; j < shortPaths.size(); ++j)
                    {
                        if (shortPaths[j].size() > longestLength)
                        {
                            longestLength = shortPaths.size();
                            longestIndex = j;
                        }
                    }

                    shortPaths.erase(shortPaths.begin() + longestIndex);
                }
            }
        }

        // Randomly pick a short path to take
        unsigned int numShortPaths = shortPaths.size();
        if (numShortPaths > 0)
        {
            unsigned int shortestIndex;
            shortestIndex = Random::Uint(0, numShortPaths - 1);
            std::list<Tile*> walkPath = shortPaths[shortestIndex];

            // If the path is a legitimate path, walk down it to the tile to be dug out
            getGameMap()->cutCorners(walkPath, mDefinition->getTilePassability());
            if (setWalkPath(walkPath, 2, false))
            {
                setAnimationState("Walk");
                pushAction(CreatureAction::walkToTile);
                return false;
            }
        }
    }

    // If we found no path, let's stop doing this
    popAction();
    return true;
}

bool Creature::handleDigTileAction()
{
    Tile* myTile = positionTile();
    if (myTile == NULL)
        return false;

    //cout << "dig ";

    // See if any of the tiles is one of our neighbors
    bool wasANeighbor = false;
    std::vector<Tile*> creatureNeighbors = myTile->getAllNeighbors();
    Player* tempPlayer = getControllingPlayer();
    for (unsigned int i = 0; i < creatureNeighbors.size() && !wasANeighbor; ++i)
    {
        if (tempPlayer == NULL)
            break;

        Tile* tempTile = creatureNeighbors[i];

        if (!tempTile->getMarkedForDigging(tempPlayer))
            continue;

        // We found a tile marked by our controlling seat, dig out the tile.

        // If the tile is a gold tile accumulate gold for this creature.
        if (tempTile->getType() == Tile::gold)
        {
            //FIXME: Make sure we can't dig gold if the creature has max gold.
            // Or let gold on the ground, until there is space so that the player
            // isn't stuck when making a way through gold.
            double tempDouble = 5 * std::min(mDefinition->getDigRate(), tempTile->getFullness());
            mGold += (int)tempDouble;
            getGameMap()->getSeatByColor(getColor())->goldMined += (int)tempDouble;
            recieveExp(5.0 * mDefinition->getDigRate() / 20.0);
        }

        // Turn so that we are facing toward the tile we are going to dig out.
        faceToward(tempTile->x, tempTile->y);

        // Dig out the tile by decreasing the tile's fullness.
        setAnimationState("Dig");
        double amountDug = tempTile->digOut(mDefinition->getDigRate(), true);
        if(amountDug > 0.0)
        {
            recieveExp(1.5 * mDefinition->getDigRate() / 20.0);

            // If the tile has been dug out, move into that tile and try to continue digging.
            if (tempTile->getFullness() < 1)
            {
                recieveExp(2.5);
                setAnimationState("Walk");

                // Remove the dig action and replace it with
                // walking to the newly dug out tile.
                //popAction();
                addDestination((Ogre::Real)tempTile->x, (Ogre::Real)tempTile->y);
                pushAction(CreatureAction::walkToTile);
            }
            //Set sound position and play dig sound.
            mSound->setPosition(getPosition());
            mSound->play(CreatureSound::DIG);
        }
        else
        {
            //We tried to dig a tile we are not able to
            //Completely bail out if this happens.
            clearActionQueue();
        }

        wasANeighbor = true;
        break;
    }

    // Check to see if we are carrying the maximum amount of gold we can carry, and if so, try to take it to a treasury.
    if (mGold >= MaxGoldCarriedByWorkers)
    {
        // Remove the dig action and replace it with a depositGold action.
        pushAction(CreatureAction::depositGold);
    }

    // If we successfully dug a tile then we are done for this turn.
    if (wasANeighbor)
        return false;

    // Find paths to all of the neighbor tiles for all of the marked visible tiles.
    std::vector<std::list<Tile*> > possiblePaths;
    std::vector<Tile*> markedTiles = getVisibleMarkedTiles();
    for (unsigned int i = 0; i < markedTiles.size(); ++i)
    {
        std::vector<Tile*> neighbors = markedTiles[i]->getAllNeighbors();
        for (unsigned int j = 0; j < neighbors.size(); ++j)
        {
            Tile* neighborTile = neighbors[j];
            if (neighborTile != NULL && neighborTile->getFullness() < 1)
                possiblePaths.push_back(getGameMap()->path(positionTile(), neighborTile, mDefinition->getTilePassability()));
        }
    }

    // Find the shortest path and start walking toward the tile to be dug out
    if (!possiblePaths.empty())
    {
        // Find the N shortest valid paths, see if there are any valid paths shorter than this first guess
        std::vector<std::list<Tile*> > shortPaths;
        for (unsigned int i = 0; i < possiblePaths.size(); ++i)
        {
            // If the current path is long enough to be valid
            unsigned int currentLength = possiblePaths[i].size();
            if (currentLength >= 2)
            {
                shortPaths.push_back(possiblePaths[i]);

                // If we already have enough short paths
                if (shortPaths.size() > 5)
                {
                    unsigned int longestLength, longestIndex;

                    // Kick out the longest
                    longestLength = shortPaths[0].size();
                    longestIndex = 0;
                    for (unsigned int j = 1; j < shortPaths.size(); ++j)
                    {
                        if (shortPaths[j].size() > longestLength)
                        {
                            longestLength = shortPaths.size();
                            longestIndex = j;
                        }
                    }

                    shortPaths.erase(shortPaths.begin() + longestIndex);
                }
            }
        }

        // Randomly pick a short path to take
        unsigned int numShortPaths = shortPaths.size();
        if (numShortPaths > 0)
        {
            unsigned int shortestIndex;
            shortestIndex = Random::Uint(0, numShortPaths - 1);
            std::list<Tile*> walkPath = shortPaths[shortestIndex];

            // If the path is a legitimate path, walk down it to the tile to be dug out
            getGameMap()->cutCorners(walkPath, mDefinition->getTilePassability());
            if (setWalkPath(walkPath, 2, false))
            {
                setAnimationState("Walk");
                pushAction(CreatureAction::walkToTile);
                return false;
            }
        }
    }

    // If none of our neighbors are marked for digging we got here too late.
    // Finish digging
    bool isDigging = (mActionQueue.front().getType() == CreatureAction::digTile);
    if (isDigging)
    {
        popAction();
        return true;
    }
    return false;
}

bool Creature::handleDepositGoldAction()
{
    // Check to see if we are standing in a treasury.
    Tile* myTile = positionTile();
    if (myTile == NULL)
        return false;

    Room* tempRoom = myTile->getCoveringRoom();
    if (tempRoom != NULL && tempRoom->getType() == Room::treasury)
    {
        // Deposit as much of the gold we are carrying as we can into this treasury.
        mGold -= static_cast<RoomTreasury*>(tempRoom)->depositGold(mGold, myTile);

        // Depending on how much gold we have left (what did not fit in this treasury) we may want to continue
        // looking for another treasury to put the gold into.  Roll a dice to see if we want to quit looking not.
        if (Random::Double(1.0, MaxGoldCarriedByWorkers) > mGold)
        {
            popAction();
            return false;
        }
    }

    // We were not standing in a treasury that has enough room for the gold we are carrying, so try to find one to walk to.
    // Check to see if our seat controls any treasuries.
    std::vector<Room*> treasuriesOwned = getGameMap()->getRoomsByTypeAndColor(Room::treasury, getColor());
    if (treasuriesOwned.empty())
    {
        // There are no treasuries available so just go back to what we were doing.
        popAction();
        LogManager::getSingleton().logMessage("No space to put gold for creature for player "
            + Ogre::StringConverter::toString(getColor()));
        return true;
    }

    Tile* nearestTreasuryTile = NULL;
    unsigned int nearestTreasuryDistance = 0;
    bool validPathFound = false;
    std::list<Tile*> tempPath;

    // Loop over the treasuries to find the closest one.
    for (unsigned int i = 0; i < treasuriesOwned.size(); ++i)
    {
        if (!validPathFound)
        {
            // We have not yet found a valid path to a treasury, check to see if we can get to this treasury.
            unsigned int tempUnsigned = Random::Uint(0, treasuriesOwned[i]->numCoveredTiles() - 1);
            nearestTreasuryTile = treasuriesOwned[i]->getCoveredTile(tempUnsigned);
            tempPath = getGameMap()->path(myTile, nearestTreasuryTile, mDefinition->getTilePassability());
            if (tempPath.size() >= 2 && static_cast<RoomTreasury*>(treasuriesOwned[i])->emptyStorageSpace() > 0)
            {
                validPathFound = true;
                nearestTreasuryDistance = tempPath.size();
            }
        }
        else
        {
            // We have already found at least one valid path to a treasury, see if this one is closer.
            unsigned int tempUnsigned = Random::Uint(0, treasuriesOwned[i]->numCoveredTiles() - 1);
            Tile* tempTile = treasuriesOwned[i]->getCoveredTile(tempUnsigned);
            std::list<Tile*> tempPath2 = getGameMap()->path(myTile, tempTile, mDefinition->getTilePassability());
            if (tempPath2.size() >= 2 && tempPath2.size() < nearestTreasuryDistance
                && static_cast<RoomTreasury*>(treasuriesOwned[i])->emptyStorageSpace() > 0)
            {
                tempPath = tempPath2;
                nearestTreasuryDistance = tempPath.size();
            }
        }
    }

    if (validPathFound)
    {
        // Begin walking to this treasury.
        getGameMap()->cutCorners(tempPath, mDefinition->getTilePassability());
        if (setWalkPath(tempPath, 2, false))
        {
            setAnimationState("Walk");
            pushAction(CreatureAction::walkToTile);
            return false;
        }
    }

    // If we get to here, there is either no treasuries controlled by us, or they are all
    // unreachable, or they are all full, so quit trying to deposit gold.
    popAction();
    LogManager::getSingleton().logMessage("No space to put gold for creature for player "
        + Ogre::StringConverter::toString(getColor()));
    return true;
}

bool Creature::handleFindHomeAction()
{
    // Check to see if we are standing in an open quarters tile that we can claim as our home.
    Tile* myTile = positionTile();
    if (myTile == NULL)
        return false;

    Room* tempRoom = myTile->getCoveringRoom();
    if (tempRoom != NULL && tempRoom->getType() == Room::quarters)
    {
        if (static_cast<RoomQuarters*>(tempRoom)->claimTileForSleeping(myTile, this))
            mHomeTile = myTile;
    }

    // If we found a tile to claim as our home in the above block.
    if (mHomeTile != NULL)
    {
        popAction();
        return true;
    }

    // Check to see if we can walk to a quarters that does have an open tile.
    std::vector<Room*> tempRooms = getGameMap()->getRoomsByTypeAndColor(Room::quarters, getColor());
    std::random_shuffle(tempRooms.begin(), tempRooms.end());
    unsigned int nearestQuartersDistance = 0;
    bool validPathFound = false;
    std::list<Tile*> tempPath;
    for (unsigned int i = 0; i < tempRooms.size(); ++i)
    {
        // Get the list of open rooms at the current quarters and check to see if
        // there is a place where we could put a bed big enough to sleep in.
        Tile* tempTile = static_cast<RoomQuarters*>(tempRooms[i])->getLocationForBed(
                        mDefinition->getBedDim1(), mDefinition->getBedDim2());

        // If the previous attempt to place the bed in this quarters failed, try again with the bed the other way.
        if (tempTile == NULL)
            tempTile = static_cast<RoomQuarters*>(tempRooms[i])->getLocationForBed(
                                                                     mDefinition->getBedDim2(), mDefinition->getBedDim1());

        // Check to see if either of the two possible bed orientations tried above resulted in a successful placement.
        if (tempTile != NULL)
        {
            std::list<Tile*> tempPath2 = getGameMap()->path(myTile, tempTile,
                    mDefinition->getTilePassability());

            // Find out the minimum valid path length of the paths determined in the above block.
            if (!validPathFound)
            {
                // If the current path is long enough to be valid then record the path and the distance.
                if (tempPath2.size() >= 2)
                {
                    tempPath = tempPath2;
                    nearestQuartersDistance = tempPath.size();
                    validPathFound = true;
                }
            }
            else
            {
                // If the current path is long enough to be valid but shorter than the
                // shortest path seen so far, then record the path and the distance.
                if (tempPath2.size() >= 2 && tempPath2.size()
                        < nearestQuartersDistance)
                {
                    tempPath = tempPath2;
                    nearestQuartersDistance = tempPath.size();
                }
            }
        }
    }

    // If we found a valid path to an open room in a quarters, then start walking along it.
    if (validPathFound)
    {
        getGameMap()->cutCorners(tempPath, mDefinition->getTilePassability());
        if (setWalkPath(tempPath, 2, false))
        {
            setAnimationState("Walk");
            pushAction(CreatureAction::walkToTile);
            return false;
        }
    }

    // If we got here there are no reachable quarters that are unclaimed so we quit trying to find one.
    popAction();
    return true;
}

bool Creature::handleTrainingAction()
{
    // Current creature tile position
    Tile* myTile = positionTile();

    // Creatures can only train to level 10 at a dojo.
    //TODO: Check to see if the dojo has been upgraded to allow training to a higher level.
    if (getLevel() > 10)
    {
        popAction();
        mTrainWait = 0;

        stopUsingDojo();
        return true;
    }
    // Randomly decide to stop training, we are more likely to stop when we are tired.
    else if (100.0 * std::pow(Random::Double(0.0, 1.0), 2) > mAwakeness)
    {
        popAction();
        mTrainWait = 0;

        stopUsingDojo();
        return true;
    }
    // Decrement a counter each turn until it reaches 0, if it reaches 0 we try to train this turn.
    else if (mTrainWait > 0)
    {
        setAnimationState("Idle");
        --mTrainWait;
    }
    // Make sure we are on the map.
    else if (myTile != NULL)
    {
        // See if we are in a dojo now.
        Room* tempRoom = myTile->getCoveringRoom();
        if (tempRoom != NULL && tempRoom->getType() == Room::dojo && tempRoom->numOpenCreatureSlots() > 0)
        {
            Tile* tempTile = tempRoom->getCentralTile();
            if (tempTile != NULL)
            {
                // Train at this dojo.
                mTrainingDojo = static_cast<RoomDojo*>(tempRoom);
                mTrainingDojo->addCreatureUsingRoom(this);
                faceToward(tempTile->x, tempTile->y);
                setAnimationState("Attack1");
                recieveExp(5.0);
                mAwakeness -= 5.0;
                mTrainWait = Random::Uint(3, 8);
            }
        }
    }
    else
    {
        // We are not on the map, don't do anything.
        popAction();

        stopUsingDojo();
        return false;
    }

    // Get the list of dojos controlled by our seat and make sure there is at least one.
    std::vector<Room*> tempRooms = getGameMap()->getRoomsByTypeAndColor(Room::dojo, getColor());

    if (tempRooms.empty())
    {
        popAction();

        stopUsingDojo();
        return true;
    }

    // Pick a dojo to train at and try to walk to it.
    //TODO: Pick a close dojo, not necessarily the closest just a somewhat closer than average one.
    int tempInt = 0;
    double maxTrainDistance = 40.0;
    Room* tempRoom = NULL;
    do
    {
        tempInt = Random::Uint(0, tempRooms.size() - 1);
        tempRoom = tempRooms[tempInt];
        tempRooms.erase(tempRooms.begin() + tempInt);
        double tempDouble = 1.0 / (maxTrainDistance - getGameMap()->crowDistance(myTile, tempRoom->getCoveredTile(0)));
        if (Random::Double(0.0, 1.0) < tempDouble)
            break;
        ++tempInt;
    } while (tempInt < 5 && tempRoom->numOpenCreatureSlots() == 0 && !tempRooms.empty());

    if (tempRoom && tempRoom->numOpenCreatureSlots() == 0)
    {
        // The room is already being used, stop trying to train.
        popAction();
        stopUsingDojo();
        return true;
    }

    Tile* tempTile = tempRoom->getCoveredTile(Random::Uint(0, tempRoom->numCoveredTiles() - 1));
    std::list<Tile*> tempPath = getGameMap()->path(myTile, tempTile, mDefinition->getTilePassability());
    if (tempPath.size() < maxTrainDistance && setWalkPath(tempPath, 2, false))
    {
        setAnimationState("Walk");
        pushAction(CreatureAction::walkToTile);
        return false;
    }
    else
    {
        // We could not find a dojo to train at so stop trying to find one.
        popAction();
    }

    // Default action
    stopUsingDojo();
    return true;
}

void Creature::stopUsingDojo()
{
    if (mTrainingDojo == NULL)
        return;

    mTrainingDojo->removeCreatureUsingRoom(this);
    mTrainingDojo = NULL;
}

bool Creature::handleAttackAction()
{
    // If there are no more enemies which are reachable, stop attacking
    if (mReachableEnemyObjects.empty())
    {
        popAction();
        return true;
    }

    // Find the first enemy close enough to hit and attack it
    if (mLivingEnemyObjectsInRange.empty())
    {
        // There is not an enemy within range, begin maneuvering to try to get near an enemy, or out of the combat situation.
        popAction();
        pushAction(CreatureAction::maneuver);
        return true;
    }

    GameEntity* tempAttackableObject = mLivingEnemyObjectsInRange[0];

    // Turn to face the creature we are attacking and set the animation state to Attack.
    //TODO:  This should be improved so it picks the closest tile rather than just the [0] tile.
    Tile* tempTile = tempAttackableObject->getCoveredTiles()[0];
    clearDestinations();
    faceToward(tempTile->x, tempTile->y);
    setAnimationState("Attack1");

    //Play attack sound
    //TODO - syncronise with animation
    mSound->setPosition(getPosition());
    mSound->play(CreatureSound::ATTACK);

    // Calculate how much damage we do.
    Tile* myTile = positionTile();
    double damageDone = getHitroll(getGameMap()->crowDistance(myTile, tempTile));
    damageDone *= Random::Double(0.0, 1.0);
    damageDone -= std::pow(Random::Double(0.0, 0.4), 2.0) * tempAttackableObject->getDefense();

    // Make sure the damage is positive.
    if (damageDone < 0.0)
        damageDone = 0.0;

    // Do the damage and award experience points to both creatures.
    tempAttackableObject->takeDamage(damageDone, tempTile);
    double expGained;
    expGained = 1.0 + 0.2 * std::pow(damageDone, 1.3);
    mAwakeness -= 0.5;

    // Give a small amount of experince to the creature we hit.
    if(tempAttackableObject->getObjectType() == GameEntity::creature)
    {
        Creature* tempCreature = static_cast<Creature*>(tempAttackableObject);
        tempCreature->recieveExp(0.15 * expGained);

        // Add a bonus modifier based on the level of the creature we hit
        // to expGained and give ourselves that much experience.
        if (tempCreature->getLevel() >= getLevel())
            expGained *= 1.0 + (tempCreature->getLevel() - getLevel()) / 10.0;
        else
            expGained /= 1.0 + (getLevel() - tempCreature->getLevel()) / 10.0;
    }
    recieveExp(expGained);

    //std::cout << "\n" << getName() << " did " << damageDone
    //        << " damage to "
            //FIXME: Attackabe object needs a name...
    //        << "";
            //<< tempAttackableObject->getName();
    //std::cout << " who now has " << tempAttackableObject->getHP(
        //       tempTile) << "hp";

    // Randomly decide to start maneuvering again so we don't just stand still and fight.
    if (Random::Double(0.0, 1.0) <= 0.6)
        popAction();

    return false;
}

bool Creature::handleManeuverAction()
{
    // If there is an enemy within range, stop maneuvering and attack it.
    if (!mLivingEnemyObjectsInRange.empty())
    {
        popAction();

        // If the next action down the stack is not an attackObject action, add it.
        bool tempBool = (mActionQueue.front().getType() != CreatureAction::attackObject);
        if (tempBool)
            pushAction(CreatureAction::attackObject);

        return true;
    }

    // If there are no more enemies which are reachable, stop maneuvering.
    if (mReachableEnemyObjects.empty())
    {
        popAction();
        return true;
    }

    // Should not happen
    if (mBattleField == NULL)
        return true;

    /*
    // TODO: Check this
    // Check to see if we should try to strafe the enemy
    if(randomDouble(0.0, 1.0) < 0.3)
    {
        //TODO:  This should be improved so it picks the closest tile rather than just the [0] tile.
        tempTile = nearestEnemyObject->getCoveredTiles()[0];
        tempVector = Ogre::Vector3(tempTile->x, tempTile->y, 0.0);
        tempVector -= position;
        tempVector.normalise();
        tempVector *= randomDouble(0.0, 3.0);
        tempQuat.FromAngleAxis(Ogre::Degree((randomDouble(0.0, 1.0) < 0.5 ? 90 : 270)), Ogre::Vector3::UNIT_Z);
        tempTile = getGameMap()->getTile(positionTile()->x + tempVector.x, positionTile()->y + tempVector.y);
        if(tempTile != NULL)
        {
            tempPath = getGameMap()->path(positionTile(), tempTile, tilePassability);

            if(setWalkPath(tempPath, 2, false))
                setAnimationState("Walk");
        }
    }
    */

    // There are no enemy creatures in range so we will have to maneuver towards one.
    // Prepare the battlefield so we can decide where to move.
    if (mBattleFieldAgeCounter == 0)
    {
        computeBattlefield();
        mBattleFieldAgeCounter = Random::Uint(2, 6);
    }

    // Find a location on the battlefield to move to, we try to find a minumum if we are
    // trying to "attack" and a maximum if we are trying to "retreat".
    Tile* myTile = positionTile();
    bool attack_animation = true;
    SecurityTile minimumFieldValue(-1, -1, 0.0);

    // Check whether the hostility level is not under zero, meaning that we have enough allies
    // around there aren't enough enemies to go and attack.
    if (mBattleField->getTileSecurityLevel(myTile->x, myTile->y) > 0.0)
    {
        minimumFieldValue = mBattleField->getMinSecurityLevel(); // Attack where there are most enemies
        attack_animation = true;
    }
    else
    {
        // Too much enemies or not enough allies
        minimumFieldValue = mBattleField->getMaxSecurityLevel(); // Retreat where there are most allies
        attack_animation = false;
    }

    // Find a path if we obtained an actual tile to it
    if (minimumFieldValue.getPosX() < 0 || minimumFieldValue.getPosY() < 0)
        return true;

    // Pick a destination tile near the tile we got from the battlefield.
    clearDestinations();
    // Pick a true destination randomly within the max range of our weapons.
    double tempDouble = std::max(mWeaponL ? mWeaponL->getRange() : 0.0,
                                 mWeaponR ? mWeaponR->getRange() : 0.0);
    tempDouble = sqrt(tempDouble);

    std::list<Tile*> tempPath = getGameMap()->path(positionTile()->x, positionTile()->y,
                                              (int)minimumFieldValue.getPosX() + Random::Double(-1.0 * tempDouble, tempDouble),
                                              (int)minimumFieldValue.getPosY() + Random::Double(-1.0 * tempDouble, tempDouble),
                                              mDefinition->getTilePassability());

    // Walk a maximum of N tiles before recomputing the destination since we are in combat.
    unsigned int tempUnsigned = 5;
    if (tempPath.size() >= tempUnsigned)
        tempPath.resize(tempUnsigned);

    getGameMap()->cutCorners(tempPath, mDefinition->getTilePassability());
    if (setWalkPath(tempPath, 2, false))
    {
        setAnimationState(attack_animation ? "Walk" : "Flee");
    }

    // Push a walkToTile action into the creature's action queue to make them walk the path they have
    // decided on without recomputing, this helps prevent them from getting stuck in local minima.
    pushAction(CreatureAction::walkToTile);

    // This is a debugging statement, it produces a visual display of the battlefield seen by the first created creature.
    //TODO: Add support to display this when toggling the debug view. See ODFrameListener / GameMode.
    /*
    if (mBattleField->getName().compare("field_1") == 0)
    {
        mBattleField->refreshMeshes(1.0);
    }*/
    return false;
}

bool Creature::handleSleepAction()
{
    Tile* myTile = positionTile();
    if (mHomeTile == NULL)
    {
        popAction();
        return false;
    }

    if (myTile != mHomeTile)
    {
        // Walk to the the home tile.
        std::list<Tile*> tempPath = getGameMap()->path(myTile, mHomeTile, mDefinition->getTilePassability());
        getGameMap()->cutCorners(tempPath, mDefinition->getTilePassability());
        if (setWalkPath(tempPath, 2, false))
        {
            setAnimationState("Walk");
            popAction();
            pushAction(CreatureAction::walkToTile);
            return false;
        }
    }
    else
    {
        // We are at the home tile so sleep.
        setAnimationState("Sleep");
        // Improve awakeness
        mAwakeness += 4.0;
        if (mAwakeness > 100.0)
            mAwakeness = 100.0;
        // Improve HP but a bit slower.
        mHp += 1.0;
        if (mHp > mMaxHP)
            mHp = mMaxHP;
        // Improve mana
        mMana += 4.0;
        if (mMana > mMaxMana)
            mMana = mMaxMana;

        if (mAwakeness >= 100.0 && mHp >= mMaxHP && mMana >= mMaxMana)
            popAction();
    }
    return false;
}


double Creature::getHitroll(double range)
{
    double tempHitroll = 1.0;

    if (mWeaponL != NULL && mWeaponL->getRange() >= range)
        tempHitroll += mWeaponL->getDamage();
    if (mWeaponR != NULL && mWeaponR->getRange() >= range)
        tempHitroll += mWeaponR->getDamage();
    tempHitroll *= log((double) log((double) getLevel() + 1) + 1);

    return tempHitroll;
}

double Creature::getDefense() const
{
    double returnValue = 3.0;
    if (mWeaponL != NULL)
        returnValue += mWeaponL->getDefense();
    if (mWeaponR != NULL)
        returnValue += mWeaponR->getDefense();

    return returnValue;
}

//! \brief Increases the creature's level, adds bonuses to stat points, changes the mesh, etc.
void Creature::doLevelUp()
{
    if (getLevel() >= MAX_LEVEL)
        return;

    setLevel(getLevel() + 1);
    //std::cout << "\n\n" << getName() << " has reached level " << getLevel() << "\n";

    if (mDefinition->isWorker())
    {
        mDigRate += 4.0 * getLevel() / (getLevel() + 5.0);
        mDanceRate += 0.12 * getLevel() / (getLevel() + 5.0);
        //std::cout << "New dig rate: " << mDigRate << "\tnew dance rate: " << mDanceRate << "\n";
    }

    mMoveSpeed += 0.4 / (getLevel() + 2.0);

    mMaxHP += mDefinition->getHpPerLevel();
    mMaxMana += mDefinition->getManaPerLevel();

    // Test the max HP/mana against their absolute class maximums
    if (mMaxHP > mDefinition->getMaxHp())
        mMaxHP = mDefinition->getMaxHp();
    if (mMaxMana > mDefinition->getMaxMana())
        mMaxMana = mDefinition->getMaxMana();

    // Scale up the mesh.
    if (isMeshExisting() && ((getLevel() <= 30 && getLevel() % 2 == 0) || (getLevel() > 30 && getLevel()
            % 3 == 0)))
    {
        Ogre::Real scaleFactor = (Ogre::Real)(1.0 + static_cast<double>(getLevel()) / 250.0);
        if (scaleFactor > 1.03)
            scaleFactor = 1.04;
        RenderRequest *request = new RenderRequest;
        request->type = RenderRequest::scaleSceneNode;
        request->p = mSceneNode;
        request->vec = Ogre::Vector3(scaleFactor, scaleFactor, scaleFactor);

        // Add the request to the queue of rendering operations to be performed before the next frame.
        RenderManager::queueRenderRequest(request);
    }
}

/*! \brief Creates a list of Tile pointers in visibleTiles
 *
 * The tiles are currently determined to be visible or not, according only to
 * the distance they are away from the creature.  Because of this they can
 * currently see through walls, etc.
 */
void Creature::updateVisibleTiles()
{
    mVisibleTiles = getGameMap()->visibleTiles(positionTile(), mDefinition->getSightRadius());
}

//! \brief Loops over the visibleTiles and adds all enemy creatures in each tile to a list which it returns.
std::vector<GameEntity*> Creature::getVisibleEnemyObjects()
{
    return getVisibleForce(getColor(), true);
}

//! \brief Loops over objectsToCheck and returns a vector containing all the ones which can be reached via a valid path.
std::vector<GameEntity*> Creature::getReachableAttackableObjects(const std::vector<GameEntity*>& objectsToCheck,
                                                                 unsigned int* minRange, GameEntity** nearestObject)
{
    std::vector<GameEntity*> tempVector;
    Tile* myTile = positionTile();
    Tile* objectTile = NULL;
    std::list<Tile*> tempPath;
    bool minRangeSet = false;

    // Loop over the vector of objects we are supposed to check.
    for (unsigned int i = 0; i < objectsToCheck.size(); ++i)
    {
        // Try to find a valid path from the tile this creature is in to the nearest tile where the current target object is.
        // TODO: This should be improved so it picks the closest tile rather than just the [0] tile.
        objectTile = objectsToCheck[i]->getCoveredTiles()[0];
        if (getGameMap()->pathExists(myTile->x, myTile->y, objectTile->x,
                objectTile->y, mDefinition->getTilePassability()))
        {
            tempVector.push_back(objectsToCheck[i]);

            if (minRange == NULL)
                continue;

            // TODO: If this could be computed without the path call that would be better.
            tempPath = getGameMap()->path(myTile, objectTile, mDefinition->getTilePassability());

            if (!minRangeSet)
            {
                *nearestObject = objectsToCheck[i];
                *minRange = tempPath.size();
                minRangeSet = true;
            }
            else
            {
                if (tempPath.size() < *minRange)
                {
                    *minRange = tempPath.size();
                    *nearestObject = objectsToCheck[i];
                }
            }
        }
    }

    //TODO: Maybe think of a better canary value for this.
    if (minRange != NULL && !minRangeSet)
        *minRange = 999999;

    return tempVector;
}

//! \brief Loops over the enemyObjectsToCheck vector and adds all enemy creatures within weapons range to a list which it returns.
std::vector<GameEntity*> Creature::getEnemyObjectsInRange(const std::vector<GameEntity*> &enemyObjectsToCheck)
{
    std::vector<GameEntity*> tempVector;

    // If there are no enemies to check we are done.
    if (enemyObjectsToCheck.empty())
        return tempVector;

    // Find our location and calculate the square of the max weapon range we have.
    Tile *myTile = positionTile();
    double weaponRangeSquared = std::max(mWeaponL ? mWeaponL->getRange() : 0.0,
                                         mWeaponR ? mWeaponR->getRange() : 0.0);
    weaponRangeSquared *= weaponRangeSquared;

    // Loop over the enemyObjectsToCheck and add any within range to the tempVector.
    for (unsigned int i = 0; i < enemyObjectsToCheck.size(); ++i)
    {
        //TODO:  This should be improved so it picks the closest tile rather than just the [0] tile.
        Tile *tempTile = enemyObjectsToCheck[i]->getCoveredTiles()[0];
        if (tempTile == NULL)
            continue;

        double rSquared = std::pow(myTile->x - tempTile->x, 2.0) + std::pow(
                myTile->y - tempTile->y, 2.0);

        if (rSquared < weaponRangeSquared)
            tempVector.push_back(enemyObjectsToCheck[i]);
    }

    return tempVector;
}

//! \brief Loops over the visibleTiles and adds all allied creatures in each tile to a list which it returns.
std::vector<GameEntity*> Creature::getVisibleAlliedObjects()
{
    return getVisibleForce(getColor(), false);
}

std::vector<Tile*> Creature::getVisibleMarkedTiles()
{
    std::vector<Tile*> tempVector;
    Player *tempPlayer = getControllingPlayer();

    // Loop over all the visible tiles.
    for (unsigned int i = 0, size = mVisibleTiles.size(); i < size; ++i)
    {
        // Check to see if the tile is marked for digging.
        if (tempPlayer != NULL && mVisibleTiles[i]->getMarkedForDigging(tempPlayer))
            tempVector.push_back(mVisibleTiles[i]);
    }

    return tempVector;
}

//! \brief Loops over the visibleTiles and adds any which are claimable walls.
std::vector<Tile*> Creature::getVisibleClaimableWallTiles()
{
    std::vector<Tile*> claimableWallTiles;

    // Loop over all the visible tiles.
    for (unsigned int i = 0, size = mVisibleTiles.size(); i < size; ++i)
    {
        // Check to see if the tile is marked for digging.
        if (mVisibleTiles[i]->isWallClaimable(getColor()))
            claimableWallTiles.push_back(mVisibleTiles[i]);
    }

    return claimableWallTiles;
}

//! \brief Loops over the visibleTiles and returns any creatures in those tiles whose color matches (or if invert is true, does not match) the given color parameter.
std::vector<GameEntity*> Creature::getVisibleForce(int color, bool invert)
{
    return getGameMap()->getVisibleForce(mVisibleTiles, color, invert);
}

//! \brief Displays a mesh on all of the tiles visible to the creature.
void Creature::createVisualDebugEntities()
{
    mHasVisualDebuggingEntities = true;
    mVisualDebugEntityTiles.clear();

    Tile *currentTile = NULL;
    updateVisibleTiles();
    for (unsigned int i = 0; i < mVisibleTiles.size(); ++i)
    {
        currentTile = mVisibleTiles[i];

        if (currentTile == NULL)
            continue;

        // Create a render request to create a mesh for the current visible tile.
        RenderRequest *request = new RenderRequest;
        request->type = RenderRequest::createCreatureVisualDebug;
        request->p = currentTile;
        request->p2 = static_cast<void*>(this);

        // Add the request to the queue of rendering operations to be performed before the next frame.
        RenderManager::queueRenderRequest(request);

        mVisualDebugEntityTiles.push_back(currentTile);
    }
}

//! \brief Destroy the meshes created by createVisualDebuggingEntities().
void Creature::destroyVisualDebugEntities()
{
    mHasVisualDebuggingEntities = false;

    Tile *currentTile = NULL;
    updateVisibleTiles();
    std::list<Tile*>::iterator itr;
    for (itr = mVisualDebugEntityTiles.begin(); itr != mVisualDebugEntityTiles.end(); ++itr)
    {
        currentTile = *itr;

        if (currentTile == NULL)
            continue;

        // Destroy the mesh for the current visible tile
        RenderRequest *request = new RenderRequest;
        request->type = RenderRequest::destroyCreatureVisualDebug;
        request->p = currentTile;
        request->p2 = static_cast<void*>(this);

        // Add the request to the queue of rendering operations to be performed before the next frame.
        RenderManager::queueRenderRequest(request);
    }

}

//! \brief Returns a pointer to the tile the creature is currently standing in.
Tile* Creature::positionTile()
{
    Ogre::Vector3 tempPosition = getPosition();

    return getGameMap()->getTile((int) (tempPosition.x), (int) (tempPosition.y));
}

//! \brief Conform: AttackableObject - Returns a vector containing the tile the creature is in.
std::vector<Tile*> Creature::getCoveredTiles()
{
    std::vector<Tile*> tempVector;
    tempVector.push_back(positionTile());
    return tempVector;
}

//! \brief Creates a string with a unique number embedded into it so the creature's name will not be the same as any other OGRE entity name.
std::string Creature::getUniqueCreatureName()
{
    static int uniqueNumber = 1;
    std::string className = mDefinition ? mDefinition->getClassName() : std::string();
    std::string tempString = className + Ogre::StringConverter::toString(uniqueNumber);
    ++uniqueNumber;

    while (getGameMap()->doesCreatureNameExist(tempString))
    {
        tempString = className + Ogre::StringConverter::toString(uniqueNumber);
        ++uniqueNumber;
    }

    return tempString;
}

void Creature::createStatsWindow()
{
    if (mStatsWindow != NULL)
        return;

    CEGUI::WindowManager* wmgr = CEGUI::WindowManager::getSingletonPtr();
    CEGUI::Window* rootWindow = CEGUI::System::getSingleton().getDefaultGUIContext().getRootWindow();

    mStatsWindow = wmgr->createWindow("OD/FrameWindow",
            std::string("Root/CreatureStatsWindows/") + getName());
    mStatsWindow->setPosition(CEGUI::UVector2(CEGUI::UDim(0.7, 0), CEGUI::UDim(0.65, 0)));

    mStatsWindow->setSize(CEGUI::USize(CEGUI::UDim(0.25, 0.3), CEGUI::UDim(0.25, 0.3)));
    //mStatsWindow->setSize(CEGUI::UVector2(CEGUI::UDim(0.25, 0), CEGUI::UDim(0.3, 0)));
    mStatsWindow->setSize(CEGUI::USize(CEGUI::UDim(0.25, 0), CEGUI::UDim(0.25, 0)));
    //mStatsWindow->setSize(CEGUI::Size<double>(0.25, 0.3));

    CEGUI::Window* textWindow = wmgr->createWindow("OD/StaticText", mStatsWindow->getName() + "TextDisplay");

    //textWindow->setPosition(CEGUI::USize(0.05,0.0), CEGUI::USize(0.15,0.0));
    textWindow->setPosition(CEGUI::UVector2(CEGUI::UDim(0.05, 0), CEGUI::UDim(0.15, 0)));

    //textWindow->setSize(CEGUI::USize(0.9, 0.8));
    textWindow->setSize(CEGUI::USize(CEGUI::UDim(0.9, 0), CEGUI::UDim(0.8, 0)));

    mStatsWindow->addChild(textWindow);
    rootWindow->addChild(mStatsWindow);
    mStatsWindow->show();

    updateStatsWindow();
}

void Creature::destroyStatsWindow()
{
    if (mStatsWindow != NULL)
    {
        mStatsWindow->destroy();
        mStatsWindow = NULL;
    }
}

void Creature::updateStatsWindow()
{
    if (mStatsWindow != NULL)
        mStatsWindow->getChild(mStatsWindow->getName() + "TextDisplay")->setText(getStatsText());
}

std::string Creature::getStatsText()
{
    std::stringstream tempSS;
    tempSS << "Creature name: " << getName() << "\n";
    tempSS << "HP: " << getHP() << " / " << mMaxHP << "\n";
    tempSS << "Mana: " << getMana() << " / " << mMaxMana << "\n";
    tempSS << "AI State: " << mActionQueue.front().toString() << "\n";
    return tempSS.str();
}

//! \brief Sets the creature definition for this creature
void Creature::setCreatureDefinition(const CreatureDefinition* def)
{
    mDefinition = def;

    if (mDefinition == NULL)
    {
        std::cout << "Invalid creature definition for creature: " << getName() << std::endl;
        return;
    }

    // Note: We reset the creature to level 1 in that case.
    setLevel(1);
    mExp = 0.0;

    mMaxHP = def->getHpPerLevel();
    setHP(mMaxHP);
    mMaxMana = def->getManaPerLevel();
    setMana(mMaxMana);
    setMoveSpeed(def->getMoveSpeed());
    mDigRate = def->getDigRate();
    mDanceRate = def->getDanceRate();
}

//! \brief Conform: AttackableObject - Deducts a given amount of HP from this creature.
void Creature::takeDamage(double damage, Tile *tileTakingDamage)
{
    mHp -= damage;
}

//! \brief Conform: AttackableObject - Adds experience to this creature.
void Creature::recieveExp(double experience)
{
    if (experience < 0)
        return;

    mExp += experience;
}

//! \brief An accessor to return whether or not the creature has OGRE entities for its visual debugging entities.
bool Creature::getHasVisualDebuggingEntities()
{
    return mHasVisualDebuggingEntities;
}

//! \brief Returns the first player whose color matches this creature's color.
//FIXME: This should be made into getControllingSeat(), when this is done it can simply be a call to GameMap::getSeatByColor().
Player* Creature::getControllingPlayer()
{
    Player *tempPlayer = NULL;

    if (getGameMap()->getLocalPlayer()->getSeat()->getColor() == getColor())
    {
        return getGameMap()->getLocalPlayer();
    }

    // Try to find and return a player with color equal to this creature's
    for (unsigned int i = 0, numPlayers = getGameMap()->numPlayers();
            i < numPlayers; ++i)
    {
        tempPlayer = getGameMap()->getPlayer(i);
        if (tempPlayer->getSeat()->getColor() == getColor())
        {
            return tempPlayer;
        }
    }

    // No player found, return NULL
    return NULL;
}

//! \brief Clears the action queue, except for the Idle action at the end.
void Creature::clearActionQueue()
{
    mActionQueue.clear();
    mActionQueue.push_front(CreatureAction::idle);
}

void Creature::pushAction(CreatureAction action)
{
    mActionQueue.push_front(action);

    updateStatsWindow();
}

void Creature::popAction()
{
    mActionQueue.pop_front();

    updateStatsWindow();
}

CreatureAction Creature::peekAction()
{
    CreatureAction tempAction = mActionQueue.front();

    return tempAction;
}

/** \brief This function loops over the visible tiles and computes a score for each one indicating how
 * friendly or hostile that tile is and stores it in the battleField variable.
 */
void Creature::computeBattlefield()
{
    if (mBattleField == NULL)
        return;

    Tile *tempTile = 0;
    int xDist, yDist = 0;
    GameEntity* tempObject = NULL;

    // Loop over the tiles in this creature's battleField and compute their value.
    // The creature will then walk towards the tile with the minimum value to
    // attack or towards the maximum value to retreat.
    mBattleField->clear();
    for (unsigned int i = 0; i < mVisibleTiles.size(); ++i)
    {
        tempTile = mVisibleTiles[i];
        double tileValue = 0.0;// - sqrt(rSquared)/sightRadius;

        // Enemies
        for (unsigned int j = 0; j < mReachableEnemyObjects.size(); ++j)
        {
            // Skip over objects which will not attack us (they either do not attack at all, or they are dead).
            tempObject = mReachableEnemyObjects[j];
            if ( ! (    tempObject->getObjectType() == GameEntity::creature
                     || tempObject->getObjectType() == GameEntity::trap)
                     || tempObject->getHP(0) <= 0.0)
            {
                continue;
            }

            //TODO:  This should be improved so it picks the closest tile rather than just the [0] tile.
            Tile *tempTile2 = tempObject->getCoveredTiles()[0];

            // Compensate for how close the creature is to me
            //rSquared = std::pow(myTile->x - tempTile2->x, 2.0) + std::pow(myTile->y - tempTile2->y, 2.0);
            //double factor = 1.0 / (sqrt(rSquared) + 1.0);

            // Subtract for the distance from the enemy creature to r
            xDist = tempTile->x - tempTile2->x;
            yDist = tempTile->y - tempTile2->y;
            tileValue -= 1.0 / sqrt((double) (xDist * xDist + yDist * yDist + 1));
        }

        // Allies
        for (unsigned int j = 0; j < mReachableAlliedObjects.size(); ++j)
        {
            //TODO:  This should be improved so it picks the closest tile rather than just the [0] tile.
            Tile *tempTile2 = mVisibleAlliedObjects[j]->getCoveredTiles()[0];

            // Compensate for how close the creature is to me
            //rSquared = std::pow(myTile->x - tempTile2->x, 2.0) + std::pow(myTile->y - tempTile2->y, 2.0);
            //double factor = 1.0 / (sqrt(rSquared) + 1.0);

            xDist = tempTile->x - tempTile2->x;
            yDist = tempTile->y - tempTile2->y;
            tileValue += 1.2 / (sqrt((double) (xDist * xDist + yDist * yDist + 1)));
        }

        const double jitter = 0.00;
        const double tileScaleFactor = 0.5;
        mBattleField->setTileSecurityLevel(tempTile->x, tempTile->y,
                                           (tileValue + Random::Double(-1.0 * jitter, jitter)) * tileScaleFactor);
    }
}
