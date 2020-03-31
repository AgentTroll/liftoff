#ifndef LIFTOFF_CLI_ROCKET_H
#define LIFTOFF_CLI_ROCKET_H

#include <vector>
#include <liftoff-physics/force_driven_body.h>
#include "thrust.h"
#include "recording_fdb.h"

class rocket : public recording_fdb {
private:
    double prop_mass;
    std::vector<engine> engines;

public:
    rocket(double rocket_dry_mass, double rocket_prop_mass, std::vector<engine> rocket_engines,
           int fdb_derivatives = 4, double fdb_time_step = 1);

    double get_mass() const override;

    double get_prop_mass() const;

    void drain_propellant(double drain_mass);

    std::vector<engine> &get_engines();
};


#endif // LIFTOFF_CLI_ROCKET_H
