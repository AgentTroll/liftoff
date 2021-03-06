#include <cmath>

namespace liftoff {
    // https://www.grc.nasa.gov/WWW/K-12/airplane/drageq.html
    double calc_drag(double cd, double rho, double v, double a) {
        return cd * rho * v * v / 2 * a;
    }

    // https://www.grc.nasa.gov/WWW/K-12/airplane/atmosmet.html#
    static double calc_rho_ideal_state(double p, double T) {
        return p / (.2869 * (T + 273.1));
    }

    // https://www.grc.nasa.gov/WWW/K-12/airplane/atmosmet.html#
    double calc_pressure_earth(double alt) {
        if (alt >= 25000) {
            double T = -131.21 + .00299 * alt;
            return 2.488 * pow((T + 273.1) / 216.6, -11.388);
        } else if (alt >= 11000 && alt < 25000) {
            return 22.65 * exp(1.73 - .000157 * alt);
        } else if (alt < 11000) {
            double T = 15.04 - .00649 * alt;
            return 101.29 * pow((T + 273.1) / 288.08, 5.256);
        }

        return -1;
    }

    // https://www.grc.nasa.gov/WWW/K-12/airplane/atmosmet.html#
    double calc_rho_earth(double alt) {
        if (alt >= 25000) {
            double T = -131.21 + .00299 * alt;
            double p = 2.488 * pow((T + 273.1) / 216.6, -11.388);
            return calc_rho_ideal_state(p, T);
        } else if (alt >= 11000 && alt < 25000) {
            double T = -56.46;
            double p = 22.65 * exp(1.73 - .000157 * alt);
            return calc_rho_ideal_state(p, T);
        } else if (alt < 11000) {
            double T = 15.04 - .00649 * alt;
            double p = 101.29 * pow((T + 273.1) / 288.08, 5.256);
            return calc_rho_ideal_state(p, T);
        }

        return -1;
    }

    double calc_drag_earth(double cd, double alt, double v, double a) {
        return calc_drag(cd, calc_rho_earth(alt), v, a);
    }
}
