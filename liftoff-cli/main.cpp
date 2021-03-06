/**
 * @file
 */

#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <thread>

#include <FL/Fl.H>
#include <mgl2/mgl.h>
#include <mgl2/fltk.h>
#include <nlohmann/json.hpp>

#include <liftoff-physics/body.h>
#include <liftoff-physics/drag.h>
#include <liftoff-physics/linalg.h>
#include <liftoff-physics/telem_proc.h>
#include <liftoff-physics/velocity_driven_body.h>

#include "c11_binary_latch.h"
#include "pidf_controller.h"
#include "rocket.h"
#include "engine.h"
#include "telemetry_flight_profile.h"
#include "velocity_flight_profile.h"

static const double TICKS_PER_SEC = 1;
static const double TIME_STEP = 1.0 / TICKS_PER_SEC;

static const double ACCEL_G = 9.80665;

// Coefficient of drag
// https://space.stackexchange.com/questions/16883/whats-the-atmospheric-drag-coefficient-of-a-falcon-9-at-launch-sub-sonic-larg#16885
static const double F9_CD = 0.25;
// Frontal surface area, m^2
// https://www.spacex.com/sites/spacex/files/falcon_users_guide_10_2019.pdf
static const double F9_A = M_PI * 2.6 * 2.6;

// Merlin 1D Max Thrust @ SL, N
// https://www.spacex.com/sites/spacex/files/falcon_users_guide_10_2019.pdf
static const double MERLIN_MAX_THRUST = 854000;
// Merlin 1D I_sp (or as good of a guess as people get), s
// https://en.wikipedia.org/wiki/Falcon_Heavy#cite_note-5
static const double MERLIN_ISP = 282;
// Merlin 1D nozzle exit area,
// Estimates: https://forum.nasaspaceflight.com/index.php?topic=32983.45
// Estimates: https://www.reddit.com/r/spacex/comments/4icycu/basic_analysis_of_the_merlin_1d_engine/d2x26pn/
// 0.95 m seems to be a fair diameter compromise
static const double MERLIN_A = M_PI * 0.475 * 0.475;

/**
 * Converts the given number of seconds to ticks used in
 * the simulation.
 *
 * @param seconds the number of seconds
 * @return the equivalent number of ticks
 */
static long double to_ticks(int seconds) {
    return seconds * TICKS_PER_SEC;
}

/**
 * Converts the given number of kilometers to the
 * equivalent number of meters.
 *
 * @param km the number of kilometers
 * @return the equivalent number of meters
 */
static double km_to_m(double km) {
    return km * 1000;
}

/**
 * Determines the sign of the given number.
 *
 * @param x the number which to determine the sign
 * @return -1 if negative, 1 if positive, 0 if 0
 */
static int signum(double x) {
    return (x > 0) - (x < 0);
}

/**
 * Helper function used to update the given MathGL window
 * from another thread. This must be called using the
 * Fl::update() function.
 *
 * @param data the pointer to the MathGL window
 */
static void update_wnd(void *data) {
    auto *gr = static_cast<mglWnd *>(data);
    gr->Update();
}

/**
 * Parses the SpaceXtract telemetry file from the given
 * path into the given flight profile.
 *
 * @param raw the raw flight profile to parse data into
 * @param path the path to the telemetry data file
 */
static void parse_telem(telemetry_flight_profile &raw, const std::string &path) {
    std::ifstream data_file{path};
    if (!data_file.good()) {
        std::cout << "Cannot find file '" << path << "'" << std::endl;
        data_file.close();
        return;
    }

    std::string line;
    while (std::getline(data_file, line)) {
        const auto &json = nlohmann::json::parse(line);
        double time = json["time"];
        double velocity = json["velocity"];
        double altitude = json["altitude"];

        raw.put_velocity(time, velocity);
        raw.put_altitude(time, km_to_m(altitude));
    }
    data_file.close();
}

/**
 * Performs the telemetry data parsing and then smooths the
 * data using interpolation and curve fitting.
 *
 * @param raw the raw telemetry data
 * @param fitted the processed data
 * @param path the path to the telemetry data file
 */
static void setup_flight_profile(telemetry_flight_profile &raw,
                                 telemetry_flight_profile &fitted,
                                 const std::string &path) {
    // JCSAT-18/KACIFIC1

    // https://everydayastronaut.com/prelaunch-preview-falcon-9-block-5-jcsat-18-kacific-1/
    raw.set_range(651000);
    parse_telem(raw, path);

    // Perform linear interpolation for velocity
    liftoff::interp_lin(fitted.get_velocities(), raw.get_velocities());

    // Find MECO/SES/SECO events
    const std::map<double, double> &v_fitted = fitted.get_velocities();
    auto meco_ptr = liftoff::find_event_time(++v_fitted.cbegin(), v_fitted, true);
    auto ses_1_ptr = liftoff::find_event_time(meco_ptr, v_fitted, false);
    auto seco_1_ptr = liftoff::find_event_time(ses_1_ptr, v_fitted, true);

    // events contains timestamps for beginning of the next leg
    // i.e. leg 1 < meco; meco <= leg 2
    std::vector<double> events = {meco_ptr->first, ses_1_ptr->first, seco_1_ptr->first};
    int n_events = events.size();

    // Perform linear interpolation for altitude
    const std::map<double, double> &alt_fitted = fitted.get_altitudes();
    liftoff::interp_lin(fitted.get_altitudes(), raw.get_altitudes());

    // Divide the data by each leg of the mission
    std::vector<std::vector<double>> times;
    std::vector<std::vector<double>> legs;
    liftoff::collect(times, legs, alt_fitted, events);

    // Step 1: Force points are the same for leg 1 and 3

    // Determine which points to force on the curve fit in
    // order to maintain the correct state between legs for
    // legs 1 and 3
    std::vector<liftoff::polynomial> alt_fit;
    for (int l = 0; l < n_events; ++l) {
        std::vector<std::pair<double, double>> force_points;
        if (l == 0) {
            liftoff::force(force_points, alt_fitted, times[1], 1);
        } else if (l == 2) {
            liftoff::force(force_points, alt_fitted, times[1], -1);
        }

        // Increase the order of the least-squares curve
        // regression
        alt_fit.push_back(liftoff::fit(4 + force_points.size(), times[l], legs[l], force_points));
    }

    // Re-write the curve-fitted values into the profile
    for (const auto &it : alt_fitted) {
        double t = it.first;
        double alt = it.second;
        for (int i = 0; i < n_events; ++i) {
            if (t < events[i]) {
                if (i == 1) {
                    break;
                }

                alt = alt_fit[i].val(t);
                if (alt < 0) {
                    alt = 0;
                }

                fitted.put_altitude(t, alt);
                break;
            }
        }
    }

    // Step 2: change the number of forced points for leg 2

    // Find the correct forced points for leg 2
    std::vector<std::pair<double, double>> force_points;
    liftoff::force(force_points, alt_fitted, times[0], -3);
    liftoff::force(force_points, alt_fitted, times[2], 3);

    // Re-write curve-fitted values for leg 2 into the
    // profile
    // Use the same order as forced points to avoid
    // deviation due to sharp changes in altitude
    liftoff::polynomial lip_fit = liftoff::lip(force_points);
    for (const auto &t : times[1]) {
        fitted.put_altitude(t, lip_fit.val(t));
    }
}

/**
 * Uses the Pythagorean theorem to determine the vertical
 * velocity based on the total velocity and altitude delta.
 *
 * This procedure supports only X and Y components.
 *
 * @param pidf the PIDF controller containing to compute
 * the altitude delta and the time step
 * @param cur_v the current velocity vector
 * @param mag_v the magnitude of the velocity for which
 * to compute the next velocity
 * @return the new desired velocity
 */
static liftoff::vector adjust_velocity(pidf_controller &pidf, const liftoff::vector &cur_v, double mag_v) {
    double target_x_velocity;
    double target_y_velocity;

    if (pidf.get_setpoint() == 0) {
        // 0 setpoint, must be around liftoff so the velocity
        // must be exactly vertical
        target_x_velocity = 0;
        target_y_velocity = mag_v;
    } else {
        double error = pidf.compute_error();
        target_y_velocity = error / pidf.get_time_step();

        // The velocity needed to reach the setpoint is
        // greater than the next velocity magnitude, so
        // set the Y velocity to the entire magnitude of
        // velocity
        if (std::abs(target_y_velocity) > mag_v) {
            target_y_velocity = signum(target_y_velocity) * mag_v;
        }

        // Otherwise, to reach the velocity magnitude,
        // there needs to be an additional horizontal
        // component
        target_x_velocity = std::sqrt((double) (mag_v * mag_v - target_y_velocity * target_y_velocity));
    }

    return {target_x_velocity, target_y_velocity, 0};
}

/**
 * Post-curve fitting adjustment to the altitude to
 * converge the velocity integral and the altitude profile.
 * Running this in a loop to determine the break-even will
 * find the "pitch time" or the time where the rocket
 * begins begin to travel horizontally rather than
 * just vertically.
 *
 * @param orig the original flight profile
 * @param fitted the curve-fit profile to write the results
 * @param break_even the time at which the velocity
 * integral and the altitude are to break even
 * @param max_time the maximum time to limit the
 * velocity integration range
 */
static void adjust_altitude(const telemetry_flight_profile &orig,
                            telemetry_flight_profile &fitted,
                            double break_even,
                            double max_time) {
    double last_t = 0;
    double last_alt = 0;
    double v_integral = 0;
    for (int i = 0; i < max_time / fitted.get_time_step(); ++i) {
        double t = i * fitted.get_time_step();
        double alt = fitted.get_altitude(t);
        double v = fitted.get_velocity(t);

        // Integrate velocity using Euler's method
        double dt = fitted.get_time_step();
        v_integral += v * dt;

        if (t < break_even) {
            // Re-fit the altitude with the velocity integral
            fitted.put_altitude(t, v_integral);
            last_alt = v_integral;
        } else {
            // Then for the rest of the profile, adjust the
            // altitude and translate downwards from the
            // original flight profile so the rest of the
            // profile connects with the velocity integral
            // break-even
            double target_error = orig.get_altitude(t) - orig.get_altitude(last_t);
            double target_alt = last_alt + target_error;
            if (target_alt >= alt) {
                break;
            }

            fitted.put_altitude(t, target_alt);
            last_alt = target_alt;
        }

        last_t = t;
    }
}

/**
 * @brief Runs the full flight profile parsed directly from
 * data and then conditioned to obtain the X/Y resulting
 * velocity profile.
 */
class run_telemetry_profile : public mglDraw {
private:
    /**
     * The resulting flight profile with the extracted
     * horizontal velocity component.
     */
    velocity_flight_profile &profile;
    /**
     * The simulation completion latch.
     */
    c11_binary_latch latch;

    /**
     * The data used for plotting on the window.
     */
    mglData plot_data;
    /**
     * The plot window to update with the data.
     */
    mglWnd *wnd_inst{nullptr};
public:
    /**
     * Creates a new instance of the telemetry data
     * plotting class with the given flight profile to
     * store the results of running the simulation.
     *
     * @param profile the result of running the simulation
     * and processing the data
     */
    explicit run_telemetry_profile(velocity_flight_profile &profile) :
            profile(profile) {
    }

    /**
     * Obtains the profile generated from running the
     * simulation.
     *
     * Not valid before get_latch().wait() returns.
     *
     * @return the X/Y profile computed from the
     * conditioned telemetry data
     */
    const velocity_flight_profile &get_profile() const {
        return profile;
    }

    /**
     * Obtains the completion latch, which will be released
     * when the simulation ends.
     *
     * @return the latch which completes at the end of the
     * telemetry replay
     */
    c11_binary_latch &get_latch() {
        return latch;
    }

    /**
     * Sets the window initialized by this MathGL drawing
     * class.
     *
     * @param wnd the pointer to the window
     */
    void set_window(mglWnd *wnd) {
        wnd_inst = wnd;
    }

    int Draw(mglGraph *gr) override {
        gr->Clf();

        gr->SubPlot(2, 2, 0);
        gr->Title("Position");
        gr->Label('x', "Downrange Distance (m)");
        gr->Label('y', "Altitude (m)");
        gr->Grid();
        gr->Box();
        gr->SetRanges(plot_data.SubData(-1, 0, 0), plot_data.SubData(-1, 0, 1));
        gr->Axis("xy");
        gr->Plot(plot_data.SubData(-1, 0, 0), plot_data.SubData(-1, 0, 1));

        gr->SubPlot(2, 2, 1);
        gr->Title("Velocity");
        gr->Label('x', "Time (s)");
        gr->Label('y', "Y Velocity (m/s)");
        gr->Grid();
        gr->Box();
        gr->SetRanges(plot_data.SubData(-1, 1, 0), plot_data.SubData(-1, 1, 1));
        gr->Axis("xy");
        gr->Plot(plot_data.SubData(-1, 1, 0), plot_data.SubData(-1, 1, 1));

        gr->SubPlot(2, 2, 2);
        gr->Title("Acceleration");
        gr->Label('x', "Time (s)");
        gr->Label('y', "Y Acceleration (m/s^2)");
        gr->Grid();
        gr->Box();
        gr->SetRanges(plot_data.SubData(-1, 2, 0), plot_data.SubData(-1, 2, 1));
        gr->Axis("xy");
        gr->Plot(plot_data.SubData(-1, 2, 0), plot_data.SubData(-1, 2, 1));

        gr->SubPlot(2, 2, 3);
        gr->Title("Jerk");
        gr->Label('x', "Time (s)");
        gr->Label('y', "Y Jerk (m/s^3)");
        gr->Grid();
        gr->Box();
        gr->SetRanges(plot_data.SubData(-1, 3, 0), plot_data.SubData(-1, 3, 1));
        gr->Axis("xy");
        gr->Plot(plot_data.SubData(-1, 3, 0), plot_data.SubData(-1, 3, 1));

        return 0;
    }

    void Calc() override {
        // Rocket setup
        // Source: https://www.spaceflightinsider.com/hangar/falcon-9/
        const double stage_1_dry_mass_kg = 25600;
        const double stage_1_fuel_mass_kg = 395700;
        const double stage_2_dry_mass_kg = 3900;
        const double stage_2_fuel_mass_kg = 92670;
        const double payload_mass_kg = 6800;

        double total_mass = stage_1_dry_mass_kg + stage_1_fuel_mass_kg +
                            stage_2_dry_mass_kg + stage_2_fuel_mass_kg +
                            payload_mass_kg;

        double time_step = 1;
        liftoff::velocity_driven_body body{total_mass, 4, time_step};

        // Flight profile setup
        telemetry_flight_profile raw{time_step};
        telemetry_flight_profile fitted{time_step};
        setup_flight_profile(raw, fitted, "./data/data.json");
        const telemetry_flight_profile orig = fitted;

        double max_time = 500;
        int total_steps = static_cast<int>(max_time / time_step);

        // Final conditioning step
        double last_corrected_time = 0;
        while (true) {
            bool valid = true;
            double last_t = 0;
            double last_alt = 0;
            for (int i = 0; i < total_steps; ++i) {
                double t = i * time_step;
                double alt = fitted.get_altitude(t);

                double dt = t - last_t;
                double target_error = alt - last_alt;
                double target_v = target_error / dt;
                double v = fitted.get_velocity(t);

                // This loop ensures that there is enough
                // velocity in order to move the rocket to
                // the next recorded altitude
                // If there is not enough velocity, the
                // flight profile's altitude needs to be
                // translated down to match with the
                // velocity integral
                // Recursively do so until we can ensure
                // the entire profile velocity/altitudes
                // match
                if (v < target_v && last_corrected_time < t) {
                    last_corrected_time = t;
                    adjust_altitude(orig, fitted, t, max_time);

                    valid = false;
                    break;
                }

                last_t = t;
                last_alt = alt;
            }

            if (valid) {
                break;
            }
        }

        const std::vector<liftoff::vector> &d_mot{body.get_d_mot()};

        // Telemetry
        const liftoff::vector &p{d_mot[0]};
        const liftoff::vector &v{d_mot[1]};
        const liftoff::vector &a{d_mot[2]};
        const liftoff::vector &j{d_mot[3]};

        std::vector<double> recorded_drag;
        recorded_drag.push_back(0);

        pidf_controller pidf{time_step, 0, 0, 0, 0};

        plot_data.Create(1, 4, 2);
        for (int i = 0; i < total_steps; ++i) {
            double cur_time_s = i * time_step;

            body.pre_compute();
            pidf.set_last_state(p.get_y());

            // Position/velocity computation
            double telem_velocity = fitted.get_velocity(cur_time_s);
            double telem_alt = fitted.get_altitude(cur_time_s);
            if (!std::isnan(telem_velocity) && !std::isnan(telem_alt)) {
                pidf.set_setpoint(telem_alt);

                const liftoff::vector &new_velocity = adjust_velocity(pidf, v, telem_velocity);
                body.set_velocity(new_velocity);
            }

            // Compute drag force
            double drag_x = liftoff::calc_drag_earth(F9_CD, p.get_y(), v.get_x(), F9_A);
            double drag_y = liftoff::calc_drag_earth(F9_CD, p.get_y(), v.get_y(), F9_A);
            liftoff::vector cur_drag{drag_x, drag_y, 0};
            recorded_drag.push_back(cur_drag.get_y());

            // Computation
            body.compute_motion();
            body.post_compute();

            // Record data into the matrix
            if (i != 0) {
                plot_data.Insert('x', i);
            }

            plot_data.Put(p.get_x(), i, 0, 0);
            plot_data.Put(p.get_y(), i, 0, 1);

            plot_data.Put(cur_time_s, i, 1, 0);
            plot_data.Put(v.magnitude(), i, 1, 1);

            plot_data.Put(cur_time_s, i, 2, 0);
            plot_data.Put(a.magnitude(), i, 2, 1);

            plot_data.Put(cur_time_s, i, 3, 0);
            plot_data.Put(j.magnitude(), i, 3, 1);

            // Check for pausing
            Check();

            // Update the window
            Fl::awake(update_wnd, wnd_inst);

            // Record data to the result profile
            profile.put_vx(cur_time_s, v.get_x());
            profile.put_vy(cur_time_s, v.get_y());

            /* std::cout << cur_time_s << ", " << v.magnitude() << ", " << p.get_y() << ", " << pidf.compute_error()
                      << ", "
                      << p.get_x() << ", " << v.get_x() << ", " << v.get_y() << ", " << a.get_x() << ", " << a.get_y()
                      << ", " << a.magnitude() << ", " << j.get_x() << ", " << j.get_y() << ", " << j.magnitude()
                      << ", "
                      << pidf.get_setpoint() << ", " << pidf.get_last_state() << ", " << raw.get_altitude(cur_time_s)
                      << ", " << raw.get_altitude(cur_time_s) - p.get_y() << std::endl; */
        }

        latch.release();
    }
};

/**
 * @brief Creates a rocket model to run the profile from
 * the flight simulation and then simulate again using
 * parameters that attempt to match the original rocket.
 */
class run_test_rocket : public mglDraw {
private:
    /**
     * The parsing flight simulation used to obtain the
     * profile to model in this simulation.
     */
    run_telemetry_profile &rtp;

    /**
     * The data used for plotting on the window.
     */
    mglData plot_data;
    /**
     * The plot window to update with the data.
     */
    mglWnd *wnd_inst{nullptr};
public:
    /**
     * Constructs the model using the profile generated by
     * the given simulation instance.
     *
     * @param rtp the simulation that produces the flight
     * profile for this model
     */
    explicit run_test_rocket(run_telemetry_profile &rtp) :
            rtp(rtp) {
    }

    /**
     * Sets the window initialized by this MathGL drawing
     * class.
     *
     * @param wnd the pointer to the window
     */
    void set_window(mglWnd *wnd) {
        wnd_inst = wnd;
    }

    int Draw(mglGraph *gr) override {
        gr->Clf();

        gr->SubPlot(2, 2, 0);
        gr->Title("Position");
        gr->Label('x', "Downrange Distance (m)");
        gr->Label('y', "Altitude (m)");
        gr->Grid();
        gr->Box();
        gr->SetRanges(plot_data.SubData(-1, 0, 0), plot_data.SubData(-1, 0, 1));
        gr->Axis("xy");
        gr->Plot(plot_data.SubData(-1, 0, 0), plot_data.SubData(-1, 0, 1));

        gr->SubPlot(2, 2, 1);
        gr->Title("Velocity");
        gr->Label('x', "Time (s)");
        gr->Label('y', "Y Velocity (m/s)");
        gr->Grid();
        gr->Box();
        gr->SetRanges(plot_data.SubData(-1, 1, 0), plot_data.SubData(-1, 1, 1));
        gr->Axis("xy");
        gr->Plot(plot_data.SubData(-1, 1, 0), plot_data.SubData(-1, 1, 1));

        gr->SubPlot(2, 2, 2);
        gr->Title("Acceleration");
        gr->Label('x', "Time (s)");
        gr->Label('y', "Y Acceleration (m/s^2)");
        gr->Grid();
        gr->Box();
        gr->SetRanges(plot_data.SubData(-1, 2, 0), plot_data.SubData(-1, 2, 1));
        gr->Axis("xy");
        gr->Plot(plot_data.SubData(-1, 2, 0), plot_data.SubData(-1, 2, 1));

        gr->SubPlot(2, 2, 3);
        gr->Title("Jerk");
        gr->Label('x', "Time (s)");
        gr->Label('y', "Y Jerk (m/s^3)");
        gr->Grid();
        gr->Box();
        gr->SetRanges(plot_data.SubData(-1, 3, 0), plot_data.SubData(-1, 3, 1));
        gr->Axis("xy");
        gr->Plot(plot_data.SubData(-1, 3, 0), plot_data.SubData(-1, 3, 1));

        return 0;
    }

    void Calc() override {
        // Wait for the simulation completion
        rtp.get_latch().wait();

        // Set up the rocket
        // These numbers come from up there ^^
        const double stage_1_dry_mass_kg = 25600;
        const double stage_1_fuel_mass_kg = 395700;
        const double stage_2_dry_mass_kg = 3900;
        const double stage_2_fuel_mass_kg = 92670;
        const double payload_mass_kg = 6800;

        double merlin_p_e = liftoff::calc_pressure_earth(0) * 1000;
        std::vector<engine> engines;
        for (int i = 0; i < 9; ++i) {
            engine e{MERLIN_MAX_THRUST, MERLIN_ISP};
            engines.push_back(e);
        }

        rocket body{stage_1_dry_mass_kg + stage_2_dry_mass_kg + payload_mass_kg + stage_2_fuel_mass_kg,
                    stage_1_fuel_mass_kg,
                    engines,
                    4, TIME_STEP};

        std::vector<liftoff::vector> &forces = body.get_forces();
        const std::vector<liftoff::vector> &d_mot{body.get_d_mot()};

        // Telemetry
        const liftoff::vector &p{d_mot[0]};
        const liftoff::vector &v{d_mot[1]};
        const liftoff::vector &a{d_mot[2]};
        const liftoff::vector &j{d_mot[3]};

        // Initial state
        liftoff::vector w{0, -ACCEL_G * body.get_mass(), 0};
        liftoff::vector n{0, ACCEL_G * body.get_mass(), 0};
        forces.push_back(w);
        forces.push_back(n);
        forces.resize(4);

        plot_data.Create(1, 4, 2);
        long double sim_duration_ticks = to_ticks(400);
        for (int i = 0; i < sim_duration_ticks; ++i) {
            double cur_time_s = i * TIME_STEP;

            // Computation
            body.pre_compute();

            // Normal force computation
            liftoff::vector new_n;
            if (p.get_y() < 0) {
                for (int k = 0; k < forces.size(); ++k) {
                    if (k == 1) {
                        continue;
                    }

                    liftoff::vector &force = forces[k];
                    if (force.get_y() < 0) {
                        new_n.add({0, -force.get_y(), 0});
                    }
                }

                // Hitting the ground
                if (v.get_y() < 0) {
                    body.set_velocity({});
                }
            }
            forces[1] = new_n;

            // Recompute weight vector
            double cur_mass = body.get_mass();
            liftoff::vector cur_weight = {0, -ACCEL_G * cur_mass, 0};
            forces[0] = cur_weight;

            // Recompute drag for new velocity
            double v_mag = v.magnitude();
            liftoff::vector cur_drag;
            if (v_mag != 0) {
                double drag = liftoff::calc_drag_earth(F9_CD, p.get_y(), v_mag, F9_A);
                cur_drag = {-v.get_x() * drag / v_mag, -v.get_y() * drag / v_mag, 0};
            }
            forces[2] = cur_drag;

            // Recompute thrust
            std::vector<engine> &cur_engines{body.get_engines()};

            // Propellant check
            double prop_rem = body.get_prop_mass();
            if (prop_rem <= 0) {
                std::cout << cur_time_s << ": No propellant" << std::endl;
                continue;
            }

            const velocity_flight_profile &profile = rtp.get_profile();
            double vx = profile.get_vx(cur_time_s);
            double vy = profile.get_vy(cur_time_s);

            // Set engine throttle
            double dvx;
            double dvy;
            double accel;
            if (!std::isnan(vx) && !std::isnan(vy)) {
                dvx = vx - v.get_x();
                dvy = vy - v.get_y();

                accel = std::sqrt(dvx * dvx + dvy * dvy);
                double f = body.get_mass() * accel;
                double f_pe = f / engines.size();

                double throttle = f / (engines.size() * MERLIN_MAX_THRUST);
                /* std::cout << cur_time_s << ", " << vx << ", " << v.get_x() << ", " << dvx << ", " << vy << ", "
                          << v.get_y()
                          << ", " << dvy << ", " << accel << ", " << body.get_mass() << ", " << f << ", " << throttle
                          << std::endl; */

                for (auto &e : cur_engines) {
                    e.set_throttle(f_pe / e.get_max_thrust());
                }
            }

            // Hardcoded MECO times
            if (cur_time_s == 155) {
                std::cout << "MECO: Remaining propellant = " << body.get_prop_mass() << " kg" << std::endl;

                // Second stage separation
                body.set_mass(body.get_mass() - stage_2_dry_mass_kg - stage_2_fuel_mass_kg - payload_mass_kg);
            }

            // Turn off engines after MECO
            if (cur_time_s > 155) {
                for (auto &e : cur_engines) {
                    e.set_throttle(0);
                }
            }

            // Compute the thrust vector and recompute the
            // rocket mass with the new throttle
            double thrust_net = 0;
            for (const auto &e : cur_engines) {
                thrust_net += e.get_thrust();

                double rate = e.get_prop_flow_rate();
                double mass_flow = rate / ACCEL_G;
                double total_prop_mass = mass_flow * TIME_STEP;
                body.drain_propellant(total_prop_mass);
            }

            liftoff::vector cur_thrust{0, thrust_net, 0};
            if (!std::isnan(vx) && !std::isnan(vy)) {
                double uax = dvx / accel;
                double uay = dvy / accel;

                cur_thrust.set({uax * thrust_net, uay * thrust_net, 0});
            }
            forces[3] = cur_thrust;

            body.compute_forces();
            body.compute_motion();
            body.post_compute();

            // Record to the data matrix
            if (i != 0) {
                plot_data.Insert('x', i);
            }

            plot_data.Put(p.get_x(), i, 0, 0);
            plot_data.Put(p.get_y(), i, 0, 1);

            plot_data.Put(cur_time_s, i, 1, 0);
            plot_data.Put(v.magnitude(), i, 1, 1);

            plot_data.Put(cur_time_s, i, 2, 0);
            plot_data.Put(a.magnitude(), i, 2, 1);

            plot_data.Put(cur_time_s, i, 3, 0);
            plot_data.Put(j.magnitude(), i, 3, 1);

            // Check for pauses
            Check();

            // Update the window with data
            Fl::awake(update_wnd, wnd_inst);

            /* std::cout << cur_time_s << ", " << v.magnitude() << ", " << p.get_y() << ", " << pidf.compute_error()
                      << ", "
                      << p.get_x() << ", " << v.get_x() << ", " << v.get_y() << ", " << a.get_x() << ", " << a.get_y()
                      << ", " << a.magnitude() << ", " << j.get_x() << ", " << j.get_y() << ", " << j.magnitude()
                      << ", "
                      << pidf.get_setpoint() << ", " << pidf.get_last_state() << ", " << raw.get_altitude(cur_time_s)
                      << ", " << raw.get_altitude(cur_time_s) - p.get_y() << std::endl; */
        }
    }
};

/**
 * Runs two flight simulations: firstly, the flight replay,
 * which will reconstruct the flight and attempt to extract
 * the velocity profile from the flight telemetry and the
 * second to model the full flight dynamics.
 *
 * @return 0 if successful
 */
int main() {
    std::cout << std::setprecision(16);

    velocity_flight_profile result{TIME_STEP};

    // Run the telemetry profile simulation and record the
    // results to the given flight profile
    run_telemetry_profile rtp{result};
    mglFLTK mgl_run_telem{&rtp, "SpaceX JCSAT-18/KACIFIC1 Flight Replay"};
    rtp.set_window(&mgl_run_telem);
    rtp.Run();

    // Attempt to simulate with the parsed flight profile
    // data with the test model
    run_test_rocket rtr{rtp};
    mglFLTK mgl_run_test{&rtr, "SpaceX JCSAT-18/KACIFIC1 Flight Sim"};
    rtr.set_window(&mgl_run_test);
    rtr.Run();

    return mgl_fltk_run();
}
