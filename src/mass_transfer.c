#include "physics_models.h"

#include <math.h>
#include <string.h>

static bool absorption(const Json *args, double seconds, MassTransfer *result)
{
    double gas_volume, liquid_volume, gas, liquid, partition, rate;
    if (!physics_scalar(args, "gas_volume_m3", 1e-9, 1e9, &gas_volume) ||
        !physics_scalar(args, "liquid_volume_m3", 1e-9, 1e9, &liquid_volume) ||
        !physics_scalar(args, "gas_kg_m3", 0, 1e4, &gas) ||
        !physics_optional(args, "liquid_kg_m3", 0, 1e4, 0, &liquid) ||
        !physics_scalar(args, "partition", 0, 1e9, &partition) ||
        !physics_scalar(args, "kla_s_inv", 0, 1e6, &rate)) return false;
    double g0 = gas * gas_volume, l0 = liquid * liquid_volume, total = g0 + l0;
    double ratio = partition * liquid_volume / gas_volume;
    double equilibrium_l = total * (ratio / (1 + ratio));
    double remaining = exp(-rate * (1 + ratio) * seconds);
    double progress = -expm1(-rate * (1 + ratio) * seconds);
    *result = (MassTransfer){.method="closed Henry partition with liquid-side kLa",
        .final_kg=l0 * remaining + equilibrium_l * progress,
        .transferred_kg=(equilibrium_l-l0) * progress, .equilibrium_kg=equilibrium_l,
        .gas_kg=g0 * remaining + (total/(1+ratio)) * progress, .closed=true};
    return true;
}

static double sheet_fraction(double fo)
{
    const double pi = 3.14159265358979323846;
    if (fo <= .005) return 4 * sqrt(fo/pi);
    double residual = 0;
    for (int i = 1; i <= 61; i += 2) {
        double term = exp(-pi*pi*i*i*fo)/(i*i);
        residual += term;
        if (term < 1e-17) break;
    }
    return fmin(1, fmax(0, 1 - 8*residual/(pi*pi)));
}

static bool uptake(const Json *args, double seconds, bool polymer, MassTransfer *result)
{
    double mass, initial, equilibrium, progress;
    if (!physics_scalar(args, "dry_mass_kg", 1e-9, 1e9, &mass) ||
        !physics_optional(args, "initial_kg_per_kg", 0, 100, 0, &initial)) return false;
    if (polymer) {
        double thickness, diffusivity;
        if (!physics_scalar(args, "thickness_m", 1e-8, 100, &thickness) ||
            !physics_scalar(args, "diffusivity_m2_s", 0, 1, &diffusivity) ||
            !physics_scalar(args, "surface_kg_per_kg", 0, 100, &equilibrium)) return false;
        progress = sheet_fraction(diffusivity*seconds/(thickness*thickness));
    } else {
        double rh, capacity, affinity, rate;
        if (!physics_scalar(args, "relative_humidity", 0, 1, &rh) ||
            !physics_scalar(args, "capacity_kg_per_kg", 0, 100, &capacity) ||
            !physics_scalar(args, "affinity", 0, 1e9, &affinity) ||
            !physics_scalar(args, "rate_s_inv", 0, 1e6, &rate) || initial > capacity) return false;
        double activity = affinity * rh;
        equilibrium = capacity * (activity/(1+activity));
        progress = -expm1(-rate*seconds);
    }
    *result = (MassTransfer){.method=polymer ? "two-sided Fickian plane sheet" : "Langmuir vapor sorption with first-order kinetics",
        .final_kg=mass*(initial*(1-progress) + equilibrium*progress),
        .transferred_kg=mass*(equilibrium-initial)*progress, .equilibrium_kg=mass*equilibrium};
    return true;
}

bool mass_transfer_tool(const char *name)
{
    return !strcmp(name, "gas_liquid_absorption") || !strcmp(name, "vapor_sorption") ||
           !strcmp(name, "polymer_moisture_uptake");
}

bool mass_transfer_run(const char *name, const Json *args, MassTransfer *result)
{
    double seconds;
    if (!mass_transfer_tool(name) || !physics_scalar(args, "seconds", 0, 1e12, &seconds)) return false;
    if (!strcmp(name, "gas_liquid_absorption")) return absorption(args, seconds, result);
    return uptake(args, seconds, !strcmp(name, "polymer_moisture_uptake"), result);
}
