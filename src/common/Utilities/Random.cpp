/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "Random.h"
#include "Errors.h"
#include "SFMTRand.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <random>

static thread_local std::unique_ptr<SFMTRand> sfmtRand;
static RandomEngine engine;

static SFMTRand* GetRng()
{
    if (!sfmtRand)
        sfmtRand = std::make_unique<SFMTRand>();

    return sfmtRand.get();
}

int32 irand(int32 min, int32 max)
{
    ASSERT(max >= min);
    std::uniform_int_distribution<int32> uid(min, max);
    return uid(engine);
}

uint32 urand(uint32 min, uint32 max)
{
    ASSERT(max >= min);
    std::uniform_int_distribution<uint32> uid(min, max);
    return uid(engine);
}

uint32 urandms(uint32 min, uint32 max)
{
    ASSERT(std::numeric_limits<uint32>::max() / Milliseconds::period::den >= max);
    return urand(min * Milliseconds::period::den, max * Milliseconds::period::den);
}

float frand(float min, float max)
{
    ASSERT(max >= min);
    std::uniform_real_distribution<float> urd(min, max);
    return urd(engine);
}

Milliseconds randtime(Milliseconds min, Milliseconds max)
{
    long long diff = max.count() - min.count();
    ASSERT(diff >= 0);
    ASSERT(diff <= (uint32)-1);
    return min + Milliseconds(urand(0, diff));
}

uint32 rand32()
{
    return GetRng()->RandomUInt32();
}

double rand_norm()
{
    std::uniform_real_distribution<double> urd;
    return urd(engine);
}

double rand_chance()
{
    std::uniform_real_distribution<double> urd(0.0, 100.0);
    return urd(engine);
}

uint32 urandweighted(size_t count, double const* chances)
{
    std::discrete_distribution<uint32> dd(chances, chances + count);
    return dd(engine);
}

// PRD constant C for a nominal chance p: the n-th roll since the last hit succeeds with C*n.
// Solved by bisection on the long-run rate 1/E[rolls per hit], tabled at 0.1% steps.
static double PrdRateFor(double c)
{
    double noHitYet = 1.0, expectedRolls = 0.0;
    for (uint32 n = 1; noHitYet > 0.0; ++n)
    {
        double p = std::min(c * n, 1.0);
        expectedRolls += n * noHitYet * p;
        noHitYet *= 1.0 - p;
    }
    return 1.0 / expectedRolls;
}

static std::array<double, 1001> const PrdTable = []
{
    std::array<double, 1001> table{};
    for (size_t i = 1; i < table.size(); ++i)
    {
        double p = i / 1000.0, lo = 0.0, hi = p;
        for (int iter = 0; iter < 40; ++iter)
        {
            double mid = (lo + hi) / 2;
            (PrdRateFor(mid) < p ? lo : hi) = mid;
        }
        table[i] = hi;
    }
    return table;
}();

bool roll_prd(float chance, uint16& misses)
{
    if (chance <= 0.0f)
        return false;
    if (chance >= 100.0f)
    {
        misses = 0;
        return true;
    }

    size_t index = std::max<size_t>(1, size_t(std::lround(chance * 10.0f)));
    if (misses < std::numeric_limits<uint16>::max())
        ++misses;

    if (rand_norm() < PrdTable[index] * misses)
    {
        misses = 0;
        return true;
    }
    return false;
}

bool roll_bag(uint8 size, uint8 wins, uint8& left, uint8& winsLeft)
{
    if (!left || left > size || winsLeft > left)
    {
        left = size;
        winsLeft = wins;
    }

    bool hit = urand(1, left) <= winsLeft;
    --left;
    if (hit)
        --winsLeft;
    return hit;
}

RandomEngine& RandomEngine::Instance()
{
    return engine;
}
