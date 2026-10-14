/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Queue.h"
#include "Log.h"
#include "PlayerbotAIConfig.h"
#include "Random.h"
#include <algorithm>
#include <cmath>
#include <vector>

void Queue::Push(ActionBasket* action)
{
    cachedSelection = nullptr;
    if (!action)
    {
        return;
    }

    for (ActionBasket* basket : actions)
    {
        if (action->getAction()->getName() == basket->getAction()->getName())
        {
            updateExistingBasket(basket, action);
            return;
        }
    }

    actions.push_back(action);
}

ActionNode* Queue::Pop()
{
    ActionBasket* basketToPop = nullptr;

    if (cachedSelection)
    {
        for (ActionBasket* basket : actions)
        {
            if (basket == cachedSelection)
            {
                basketToPop = cachedSelection;
                break;
            }
        }
        cachedSelection = nullptr;
    }

    if (!basketToPop)
    {
        basketToPop = findHighestRelevanceBasket();
    }

    if (!basketToPop)
    {
        return nullptr;
    }

    return extractAndDeleteBasket(basketToPop);
}

ActionBasket* Queue::Peek()
{
    if (cachedSelection)
    {
        for (ActionBasket* basket : actions)
        {
            if (basket == cachedSelection)
                return cachedSelection;
        }
        cachedSelection = nullptr;
    }

    cachedSelection = findHighestRelevanceBasket();
    return cachedSelection;
}

uint32 Queue::Size()
{
    return actions.size();
}

void Queue::RemoveExpired()
{
    if (!sPlayerbotAIConfig.expireActionTime)
    {
        return;
    }

    std::list<ActionBasket*> expiredBaskets;
    collectExpiredBaskets(expiredBaskets);
    removeAndDeleteBaskets(expiredBaskets);
}

// Private helper methods
void Queue::updateExistingBasket(ActionBasket* existing, ActionBasket* newBasket)
{
    if (existing->getRelevance() < newBasket->getRelevance())
    {
        existing->setRelevance(newBasket->getRelevance());
    }

    if (ActionNode* actionNode = newBasket->getAction())
    {
        delete actionNode;
    }

    delete newBasket;
}

ActionBasket* Queue::findHighestRelevanceBasket() const
{
    if (actions.empty())
    {
        return nullptr;
    }

    float maxRelevance = -1.0f;
    ActionBasket* selection = nullptr;

    for (ActionBasket* basket : actions)
    {
        if (!basket)
        {
            continue;
        }

        if (basket->getRelevance() > maxRelevance)
        {
            maxRelevance = basket->getRelevance();
            selection = basket;
        }
    }

    if (!selection || maxRelevance <= 0.0f)
    {
        return selection;
    }

    // Pillar IV: Stochastic Utility / Softmax Selection for Dynamic Rotations
    // High-priority emergency actions (>= 40.0f) are kept 100% deterministic (interrupts, heals, immunities).
    // Standard rotation actions within 10% of maximum relevance are sampled with softmax temperature.
    if (sPlayerbotAIConfig.stochasticUtilitySelection && maxRelevance < 40.0f && actions.size() > 1)
    {
        float const threshold = maxRelevance * 0.90f;
        std::vector<ActionBasket*> candidates;
        std::vector<float> weights;
        float totalWeight = 0.0f;
        float temperature = std::max(0.01f, sPlayerbotAIConfig.stochasticUtilityTemperature);

        for (ActionBasket* basket : actions)
        {
            if (basket && basket->getRelevance() >= threshold)
            {
                candidates.push_back(basket);
                float weight = std::exp((basket->getRelevance() - maxRelevance) / temperature);
                weights.push_back(weight);
                totalWeight += weight;
            }
        }

        if (candidates.size() > 1 && totalWeight > 0.0f)
        {
            float roll = frand(0.0f, totalWeight);
            float accum = 0.0f;
            for (size_t i = 0; i < candidates.size(); ++i)
            {
                accum += weights[i];
                if (roll <= accum)
                    return candidates[i];
            }
        }
    }

    return selection;
}

ActionNode* Queue::extractAndDeleteBasket(ActionBasket* basket)
{
    if (cachedSelection == basket)
    {
        cachedSelection = nullptr;
    }

    ActionNode* action = basket->getAction();
    actions.remove(basket);
    delete basket;
    return action;
}

void Queue::collectExpiredBaskets(std::list<ActionBasket*>& expiredBaskets)
{
    uint32 expiryTime = sPlayerbotAIConfig.expireActionTime;
    for (ActionBasket* basket : actions)
    {
        if (basket->isExpired(expiryTime))
        {
            expiredBaskets.push_back(basket);
        }
    }
}

void Queue::removeAndDeleteBaskets(std::list<ActionBasket*>& basketsToRemove)
{
    cachedSelection = nullptr;
    for (ActionBasket* basket : basketsToRemove)
    {
        actions.remove(basket);

        if (ActionNode* action = basket->getAction())
        {
            delete action;
        }

        delete basket;
    }
}
