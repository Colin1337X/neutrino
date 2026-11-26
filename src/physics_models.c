#include "physics_models.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static double norm(const double v[3])
{
    return hypot(hypot(v[0], v[1]), v[2]);
}

static bool dimensions(const Json *args, double size[3])
{
    if (!physics_vector(args, "size", size)) return false;
    for (int axis = 0; axis < 3; ++axis)
        if (size[axis] < .001 || size[axis] > 1000) return false;
    return true;
}

static bool elastic(const Json *args, Text *out)
{
    double size[3], force[3], young, poisson;
    if (!dimensions(args, size) || !physics_vector(args, "force", force) ||
        !physics_scalar(args, "young_pa", 1, 1e15, &young) ||
        !physics_optional(args, "poisson", -.99, .499, .3, &poisson)) return false;
    const char *model = js(args,"model");
    if (!strcmp(model,"cantilever")) {
        const double length=size[0], width=size[1], depth=size[2], area=width*depth;
        if (length < 5*fmax(width,depth)) return false;
        const double iy=width*depth*depth*depth/12, iz=depth*width*width*width/12;
        const double dx=force[0]*length/(young*area);
        const double dy=force[1]*length*length*length/(3*young*iz);
        const double dz=force[2]*length*length*length/(3*young*iy);
        const double stress = fabs(force[0])/area + fabs(force[1])*length*width/(2*iz) +
                              fabs(force[2])*length*depth/(2*iy);
        if (hypot(hypot(dx,dy),dz) > .05*length || stress/young > .05) return false;
        format(out,"3D elastic cantilever along X: tip displacement [%.8g, %.8g, %.8g] m, "
                   "peak root normal stress %.8g Pa.",dx,dy,dz,stress);
        return true;
    }
    if (at(args,"model") && strcmp(model,"solid")) return false;
    double volume = size[0] * size[1] * size[2], stress[3], strain[3];
    for (int axis = 0; axis < 3; ++axis) stress[axis] = force[axis] * size[axis] / volume;
    double sum = stress[0] + stress[1] + stress[2];
    for (int axis = 0; axis < 3; ++axis) {
        strain[axis] = (stress[axis] - poisson * (sum - stress[axis])) / young;
        if (fabs(strain[axis]) > .05) return false;
    }
    format(out, "3D linear elastic solid: stress [%.8g, %.8g, %.8g] Pa; strain [%.8g, %.8g, %.8g]; "
           "deformed size [%.8g, %.8g, %.8g] m.", stress[0],stress[1],stress[2],strain[0],strain[1],strain[2],
           size[0]*(1+strain[0]),size[1]*(1+strain[1]),size[2]*(1+strain[2]));
    return true;
}

typedef struct { double position[3], previous[3], velocity[3]; } SoftNode;
typedef struct { int offset[3]; double rest; } SoftSpring;

static bool soft_count(const Json *value, size_t minimum, size_t *count)
{
    const size_t maximum = SIZE_MAX / sizeof(SoftNode);
    if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) || value->valuedouble < (double)minimum ||
        value->valuedouble > fmin((double)maximum, 9007199254740991.0) ||
        floor(value->valuedouble) != value->valuedouble) return false;
    *count = (size_t)value->valuedouble;
    return *count <= maximum;
}

static bool soft_dimensions(const Json *args, size_t shape[3], size_t *count)
{
    const size_t maximum = SIZE_MAX / sizeof(SoftNode);
    const Json *nodes = at(args, "nodes"), *resolution = at(args, "resolution");
    if (nodes) {
        if (resolution || !soft_count(nodes, 8, count)) return false;
        size_t side = (size_t)cbrt((double)*count);
        while (side*side > *count/side) --side;
        while ((side+1)*(side+1) <= *count/(side+1)) ++side;
        shape[0] = shape[1] = side;
        const size_t layer = side*side;
        shape[2] = 1 + (*count-1)/layer;
        return true;
    }
    *count = 1;
    if (cJSON_IsArray(resolution) && cJSON_GetArraySize(resolution) != 3) return false;
    for (int axis = 0; axis < 3; ++axis) {
        const Json *value = cJSON_IsArray(resolution) ? cJSON_GetArrayItem(resolution, axis) : resolution;
        shape[axis] = 2;
        if (value && !soft_count(value, 2, &shape[axis])) return false;
        if (shape[axis] > maximum / *count) return false;
        *count *= shape[axis];
    }
    return true;
}

static void soft_rest(size_t node, const size_t shape[3], const double spacing[3], double position[3])
{
    position[0] = (node % shape[0]) * spacing[0];
    position[1] = ((node / shape[0]) % shape[1]) * spacing[1];
    position[2] = (node / (shape[0] * shape[1])) * spacing[2];
}

static void soft_project(SoftNode *nodes, size_t count, const size_t shape[3], const SoftSpring *springs,
                         size_t spring_count, size_t fixed, double compliance, bool reverse)
{
    const size_t layer = shape[0]*shape[1];
    for (size_t step = 0; step < count; ++step) {
        const size_t node = reverse ? count-1-step : step;
        const size_t cell[3] = {node%shape[0], (node/shape[0])%shape[1], node/layer};
        for (size_t i = 0; i < spring_count; ++i) {
            const SoftSpring *spring = &springs[reverse ? spring_count-1-i : i];
            size_t other[3];
            bool inside = true;
            for (int axis = 0; axis < 3; ++axis) {
                const int shift = spring->offset[axis];
                if ((shift < 0 && !cell[axis]) || (shift > 0 && cell[axis]+1 == shape[axis])) {
                    inside = false;
                    break;
                }
                other[axis] = shift < 0 ? cell[axis]-1 : cell[axis]+(size_t)shift;
            }
            if (!inside) continue;
            const size_t adjacent = (other[2]*shape[1]+other[1])*shape[0]+other[0];
            if (adjacent < fixed || adjacent >= count) continue;
            double delta[3];
            for (int axis = 0; axis < 3; ++axis)
                delta[axis] = nodes[adjacent].position[axis] - nodes[node].position[axis];
            const double length = norm(delta);
            if (length <= 1e-12) continue;
            /* One compliant position projection per spring; the base has zero inverse mass. */
            const bool mobile = node >= fixed;
            const double correction = (length - spring->rest) / ((1 + mobile + compliance)*length);
            for (int axis = 0; axis < 3; ++axis) {
                double displacement = correction*delta[axis];
                if (mobile) nodes[node].position[axis] += displacement;
                nodes[adjacent].position[axis] -= displacement;
            }
        }
    }
}

static bool softbody(const Json *args, double gravity, Text *out, bool *unavailable)
{
    double size[3], load[3] = {0}, mass, stiffness, damping, seconds;
    double initial[3] = {0}, acceleration[3] = {0,0,-gravity};
    size_t shape[3], count, probe = 0;
    const Json *sample = at(args, "sample_node");
    const char *memory = js(args, "memory");
    const char *pin = js(args,"pin");
    if (!dimensions(args,size) || (at(args,"force") && !physics_vector(args,"force",load)) ||
        !physics_scalar(args,"mass_kg",.01,1e5,&mass) || !physics_scalar(args,"stiffness",.01,1e6,&stiffness) ||
        !physics_optional(args,"damping",0,100,2,&damping) || !physics_optional(args,"seconds",0,20,1,&seconds) ||
        (at(args,"memory") && strcmp(memory,"auto") && strcmp(memory,"mapped")) ||
        (at(args,"pin") && strcmp(pin,"base") && strcmp(pin,"none")) ||
        (at(args,"initial_velocity") && !physics_vector(args,"initial_velocity",initial)) ||
        (at(args,"gravity") && !physics_vector(args,"gravity",acceleration)) ||
        !soft_dimensions(args,shape,&count) || (sample && (!soft_count(sample,0,&probe) || probe >= count))) return false;

    const size_t layer = shape[0]*shape[1], top = layer*(shape[2]-1), top_count = count-top;
    const size_t fixed = !strcmp(pin,"none") ? 0 : layer;
    double spacing[3];
    for (int axis = 0; axis < 3; ++axis) spacing[axis] = size[axis]/(shape[axis]-1);
    double centroid[3] = {0};
    const char *description = "initial geometry";
    bool mapped = false;
    SoftNode *nodes = NULL;
    if (seconds > 0) {
        nodes = physics_memory_alloc(count*sizeof *nodes,!strcmp(memory,"mapped"),&mapped,&description);
        if (!nodes) {
            if (unavailable) *unavailable = true;
            format(out,"Could not allocate %zu bytes for all %zu soft-body nodes.",count*sizeof *nodes,count);
            return true;
        }
        for (size_t node = 0; node < count; ++node) {
            soft_rest(node,shape,spacing,nodes[node].position);
            if (node >= fixed) memcpy(nodes[node].velocity,initial,sizeof initial);
        }
        SoftSpring springs[13];
        size_t spring_count = 0;
        for (int z=0; z<=1; ++z) for (int y=-1; y<=1; ++y) for (int x=-1; x<=1; ++x) {
            if (!z && (y<0 || (!y && x<=0))) continue;
            double rest[3] = {x*spacing[0],y*spacing[1],z*spacing[2]};
            springs[spring_count++] = (SoftSpring){{x,y,z},norm(rest)};
        }
        const int steps = (int)fmax(1,ceil(seconds*120));
        const double dt = seconds/steps, decay = exp(-damping*dt);
        const double node_mass = mass/count, compliance = node_mass/(stiffness*dt*dt);
        for (int step = 0; step < steps; ++step) {
            for (size_t node = fixed; node < count; ++node) {
                for (int axis = 0; axis < 3; ++axis) {
                    double a = acceleration[axis];
                    if (node >= top) a += load[axis]/(top_count*node_mass);
                    nodes[node].previous[axis] = nodes[node].position[axis];
                    nodes[node].velocity[axis] = decay*(nodes[node].velocity[axis]+dt*a);
                    nodes[node].position[axis] += dt*nodes[node].velocity[axis];
                }
            }
            soft_project(nodes,count,shape,springs,spring_count,fixed,compliance,(step & 1) != 0);
            for (size_t node = fixed; node < count; ++node) for (int axis = 0; axis < 3; ++axis) {
                double position = nodes[node].position[axis];
                if (!isfinite(position) || fabs(position) > 1e6) {
                    physics_memory_free(nodes,mapped);
                    if (unavailable) *unavailable = true;
                    format(out,"Soft-body state became invalid while integrating all %zu nodes.",count);
                    return true;
                }
                nodes[node].velocity[axis] = (position-nodes[node].previous[axis])/dt;
            }
        }
        for (size_t node = top; node < count; ++node) {
            double initial[3];
            soft_rest(node,shape,spacing,initial);
            for (int axis = 0; axis < 3; ++axis)
                centroid[axis] += (nodes[node].position[axis]-initial[axis])/top_count;
        }
    }
    format(out,"3D %s soft body (%zu mass nodes, %s): top displacement [%.8g, %.8g, %.8g] m after %.8g s.",
           fixed ? "pinned" : "free",count,description,centroid[0],centroid[1],centroid[2],seconds);
    for (size_t node = top; node < count && node-top < 8; ++node) {
        double position[3];
        if (nodes) memcpy(position,nodes[node].position,sizeof position);
        else soft_rest(node,shape,spacing,position);
        format(out,"\nNode %zu [%.7g, %.7g, %.7g] m.",node,position[0],position[1],position[2]);
    }
    if (sample) {
        double position[3];
        if (nodes) memcpy(position,nodes[probe].position,sizeof position);
        else soft_rest(probe,shape,spacing,position);
        format(out,"\nSampled node %zu [%.7g, %.7g, %.7g] m.",probe,position[0],position[1],position[2]);
    }
    physics_memory_free(nodes,mapped);
    return true;
}

static bool trajectory(const Json *args, double gravity, Text *out)
{
    double origin[3], velocity[3], wind[3] = {0}, seconds, drag;
    if (!physics_vector(args, "origin", origin) || !physics_vector(args, "velocity", velocity) ||
        (at(args, "wind") && !physics_vector(args, "wind", wind)) ||
        !physics_scalar(args, "seconds", 0, 600, &seconds) ||
        !physics_optional(args, "linear_drag", 0, 100, 0, &drag)) return false;
    double position[3], final[3], weight = drag*seconds, attenuation = exp(-weight);
    double travel = weight < 1e-4 ? seconds*(1-weight/2+weight*weight/6) : -expm1(-weight)/drag;
    double gravity_travel = weight < 1e-4 ? seconds*seconds*(.5-weight/6+weight*weight/24) : (seconds-travel)/drag;
    for (int axis = 0; axis < 3; ++axis) {
        double acceleration = axis == 2 ? -gravity : 0;
        position[axis] = origin[axis] + wind[axis]*seconds + (velocity[axis]-wind[axis])*travel +
                         acceleration*gravity_travel;
        final[axis] = wind[axis] + (velocity[axis]-wind[axis])*attenuation + acceleration*travel;
    }
    format(out, "3D free-flight trajectory: position [%.8g, %.8g, %.8g] m; velocity [%.8g, %.8g, %.8g] m/s at %.8g s.",
           position[0],position[1],position[2],final[0],final[1],final[2],seconds);
    return true;
}

static bool thermal(const Json *args, Text *out)
{
    double kelvin, ambient, capacity, conductance, power, seconds;
    if (!physics_scalar(args, "kelvin", 1, 5000, &kelvin) ||
        !physics_scalar(args, "ambient_kelvin", 1, 5000, &ambient) ||
        !physics_scalar(args, "heat_capacity_j_k", .001, 1e12, &capacity) ||
        !physics_optional(args, "conductance_w_k", 0, 1e8, 0, &conductance) ||
        !physics_optional(args, "power_w", -1e8, 1e8, 0, &power) ||
        !physics_scalar(args, "seconds", 0, 86400, &seconds)) return false;
    double factor = conductance*seconds/capacity;
    double time = factor < 1e-5 ? seconds/capacity*(1-factor/2+factor*factor/6) : -expm1(-factor)/conductance;
    double final = kelvin + ((ambient-kelvin)*conductance+power)*time;
    if (final <= 0) return false;
    double size[3], probe[3], variation[3], diffusivity, sampled = final;
    const bool spatial = at(args,"size") || at(args,"probe") || at(args,"variation_k") || at(args,"thermal_diffusivity");
    if (spatial) {
        if (!dimensions(args,size) || !physics_vector(args,"probe",probe) ||
            !physics_vector(args,"variation_k",variation) ||
            !physics_scalar(args,"thermal_diffusivity",0,100,&diffusivity)) return false;
        double amplitude = 0;
        for (int axis=0; axis<3; ++axis) {
            if (probe[axis]<0 || probe[axis]>size[axis]) return false;
            amplitude += fabs(variation[axis]);
            const double k = 3.141592653589793 / size[axis];
            sampled += variation[axis] * cos(k*probe[axis]) *
                       exp(-diffusivity*k*k*seconds-factor);
        }
        if (fmin(kelvin,final) <= amplitude) return false;
    }
    double moles, volume;
    bool gas = at(args, "moles") || at(args, "volume_m3");
    if (gas && (!physics_scalar(args, "moles", 0, 1e8, &moles) ||
                !physics_scalar(args, "volume_m3", .001, 1e9, &volume))) return false;
    format(out, "Lumped thermal balance: final %.8g K, internal energy change %.8g J at %.8g s.",
           final, capacity*(final-kelvin), seconds);
    if (gas) format(out, " Constant-volume ideal-gas pressure %.8g kPa.", moles*8.314462618*final/volume/1000);
    if (spatial) format(out, " 3D insulated conduction modes: probe %.8g K.", sampled);
    return true;
}

static bool flow(const Json *args, Text *out, bool *unavailable)
{
    double size[3], probe[3];
    FlowConfig config;
    const char *medium = js(args, "medium");
    if ((at(args, "medium") && strcmp(medium,"liquid") && strcmp(medium,"gas")) ||
        !dimensions(args,size) || !flow_parse(args,size,&config)) return false;
    memcpy(probe, config.center, sizeof probe);
    if (at(args, "probe") && !physics_vector(args,"probe",probe)) return false;
    for(int axis=0; axis<3; ++axis) if(probe[axis]<0 || probe[axis]>size[axis]) return false;
    FlowField field = flow_solve(&config);
    if (config.porous && !field.grid) {
        if (unavailable) *unavailable = true;
        format(out, "Porous CFD unavailable. %s.", field.fallback ? field.fallback : "No grid");
        flow_release(&field);
        return true;
    }
    format(out, "3D %s flow: %s, %s; tracer at probe %.8g after %.8g s.",
           *medium ? medium : "liquid", field.method, field.memory, flow_sample(&field,probe), config.seconds);
    if (field.grid) format(out, " Grid %d^3, mean speed %.7g m/s, divergence RMS %.7g 1/s, mean tracer %.7g.",
                           config.resolution,field.speed,field.divergence,field.tracer_mean);
    if (field.fallback) format(out, " Fallback: %s.", field.fallback);
    flow_porous_summary(&field, out);
    flow_release(&field);
    return true;
}

static bool ignition(const Json *args, Text *out)
{
    double source[3], site[3], radius, drive, initial, on, off, pulse, cool, response;
    if (!physics_vector(args,"source",source) || !physics_vector(args,"site",site) ||
        !physics_optional(args,"reach",.001,1000,1,&radius) ||
        !physics_optional(args,"drive",0,1,1,&drive) ||
        !physics_optional(args,"initial",0,1,0,&initial) ||
        !physics_optional(args,"activate",.001,1,.6,&on) ||
        !physics_optional(args,"reset",.001,1,.3,&off) || off >= on ||
        !physics_optional(args,"pulse_ticks",0,1e6,1,&pulse) ||
        !physics_optional(args,"cool_ticks",0,1e6,0,&cool) ||
        !physics_optional(args,"response",.001,1000,1,&response)) return false;
    double delta[3];
    for (int axis=0; axis<3; ++axis) delta[axis] = (site[axis]-source[axis])/radius;
    const double distance = norm(delta);
    const double target = drive*exp(-.5*distance*distance);
    const double peak = initial+(target-initial)*(-expm1(-response*pulse));
    const double final = peak*exp(-response*cool);
    bool active = initial >= on;
    format(out,"3D fictional ignition response (normalized): pulse %.6g, after cooldown %.6g.",peak,final);
    if (!active && target > on && peak >= on) {
        active = true;
        double tick = -log1p(-(on-initial)/(target-initial))/response;
        format(out," Ignition at %.6g ticks.",tick);
    } else if (active) append(out," Initially active.");
    if (active && (peak <= off || final <= off)) {
        double tick = peak <= off ? -log((off-target)/(initial-target))/response :
                                    pulse+log(peak/off)/response;
        if (isfinite(tick)) format(out," Reset at %.6g ticks.",tick);
        else append(out," Reset during cooldown.");
        active = false;
    }
    format(out," Final state: %s.",active ? "active" : "inactive");
    return true;
}

bool physics_model_tool(const char *name)
{
    return !strcmp(name,"elastic3d") || !strcmp(name,"softbody3d") || !strcmp(name,"trajectory3d") ||
           !strcmp(name,"thermal3d") || !strcmp(name,"flow3d") || !strcmp(name,"ignition3d");
}

bool physics_model_run(const char *name, const Json *args, double gravity, Text *out, bool *unavailable)
{
    if (!strcmp(name,"elastic3d")) return elastic(args,out);
    if (!strcmp(name,"softbody3d")) return softbody(args,gravity,out,unavailable);
    if (!strcmp(name,"trajectory3d")) return trajectory(args,gravity,out);
    if (!strcmp(name,"thermal3d")) return thermal(args,out);
    if (!strcmp(name,"flow3d")) return flow(args,out,unavailable);
    if (!strcmp(name,"ignition3d")) return ignition(args,out);
    return false;
}
