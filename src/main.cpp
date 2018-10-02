#include "Eigen-3.3/Eigen/Core"
#include "Eigen-3.3/Eigen/QR"
#include "MPC.h"
#include "Model.h"
#include "json.hpp"
#include <chrono>
#include <iostream>
#include <math.h>
#include <string>
#include <thread>
#include <uWS/uWS.h>
#include <vector>
using namespace std;

/*! \mainpage
Code base being developed to interface with the Udacity Unity3d Self-driving car
simulator (available in my repo:
https://github.com/nikolaimatni/CppFun/tree/master/SelfDrivingCar).  For now we
are working with the default MPC track (term 2, project 5), but we will
ultimately customize.

The basic work flow here is you need to implement a realization of the virtual
base class Model: the pure virtual functions there are Cost, TerminalCost and
DynamicsF.  This is pretty self-explanatory and further documented in the Model
page.  Once this is specified, you initiate an MPC object with the suitable
problem parameters and Model in main.cpp and the rest of the code takes care of
the low-level details of interfacing with IpOPT and CppAD.

See https://github.com/udacity/CarND-MPC-Project/ for the dependencies that you
need to install to get this to run.

 */

// for convenience
using json = nlohmann::json;

// For converting back and forth between radians and degrees.
constexpr double pi() { return M_PI; }
double deg2rad(double x) { return x * pi() / 180; }
double rad2deg(double x) { return x * 180 / pi(); }

// Checks if the SocketIO event has JSON data.
// If there is data the JSON object in string format will be returned,
// else the empty string "" will be returned.
string hasData(string s) {
  auto found_null = s.find("null");
  auto b1 = s.find_first_of("[");
  auto b2 = s.rfind("}]");
  if (found_null != string::npos) {
    return "";
  } else if (b1 != string::npos && b2 != string::npos) {
    return s.substr(b1, b2 - b1 + 2);
  }
  return "";
}

// Evaluate a polynomial.
double polyeval(Eigen::VectorXd coeffs, double x) {
  double result = 0.0;
  for (int i = 0; i < coeffs.size(); i++) {
    result += coeffs[i] * pow(x, i);
  }
  return result;
}

// Fit a polynomial.
// Adapted from
// https://github.com/JuliaMath/Polynomials.jl/blob/master/src/Polynomials.jl#L676-L716
Eigen::VectorXd polyfit(Eigen::VectorXd xvals, Eigen::VectorXd yvals,
                        int order) {
  assert(xvals.size() == yvals.size());
  assert(order >= 1 && order <= xvals.size() - 1);
  Eigen::MatrixXd A(xvals.size(), order + 1);

  for (int i = 0; i < xvals.size(); i++) {
    A(i, 0) = 1.0;
  }

  for (int j = 0; j < xvals.size(); j++) {
    for (int i = 0; i < order; i++) {
      A(j, i + 1) = A(j, i) * xvals(j);
    }
  }

  auto Q = A.householderQr();
  auto result = Q.solve(yvals);
  return result;
}

int main() {
  uWS::Hub h;

  Bounds bounds;

  bounds.x_up_ = vector<double>(6, 1e19);
  bounds.x_low_ = vector<double>(6, -1e19);
  bounds.u_up_ = vector<double>{deg2rad(25), 1};
  bounds.u_low_ = vector<double>{-1 * deg2rad(25), -1};

  // MPC is initialized here!
  BikeModel bike_model(10, 6, 2, 1, 0.1, 70);
  Model &model = bike_model;

  MPC mpc(10, model, 10 * 6 + 9 * 2, 10 * 6, bounds);

  h.onMessage([&mpc, &bike_model](uWS::WebSocket<uWS::SERVER> ws, char *data,
                                  size_t length, uWS::OpCode opCode) {
    // "42" at the start of the message means there's a websocket message event.
    // The 4 signifies a websocket message
    // The 2 signifies a websocket event
    string sdata = string(data).substr(0, length);
    // cout << sdata << endl;
    if (sdata.size() > 2 && sdata[0] == '4' && sdata[1] == '2') {
      string s = hasData(sdata);
      if (s != "") {
        auto j = json::parse(s);
        string event = j[0].get<string>();
        if (event == "telemetry") {
          // j[1] is the data JSON object
          vector<double> ptsx = j[1]["ptsx"];
          vector<double> ptsy = j[1]["ptsy"];
          double px = j[1]["x"];
          double py = j[1]["y"];
          double psi = j[1]["psi"];
          double v = j[1]["speed"];

          /*
           * TODO: Calculate steering angle and throttle using MPC.
           *
           * Both are in between [-1, 1].
           *
           */

          vector<double> waypoints_x;
          vector<double> waypoints_y;

          // transform waypoints to be from car's perspective
          // this means we can consider px = 0, py = 0, and psi = 0
          // greatly simplifying future calculations
          for (int i = 0; i < ptsx.size(); i++) {
            double dx = ptsx[i] - px;
            double dy = ptsy[i] - py;
            waypoints_x.push_back(dx * cos(-psi) - dy * sin(-psi));
            waypoints_y.push_back(dx * sin(-psi) + dy * cos(-psi));
          }

          double *ptrx = &waypoints_x[0];
          double *ptry = &waypoints_y[0];
          Eigen::Map<Eigen::VectorXd> waypoints_x_eig(ptrx, 6);
          Eigen::Map<Eigen::VectorXd> waypoints_y_eig(ptry, 6);

          auto coeffs = polyfit(waypoints_x_eig, waypoints_y_eig, 3);
          double cte = polyeval(coeffs, 0); // px = 0, py = 0
          double epsi = -atan(coeffs[1]);   // p

          double steer_value = j[1]["steering_angle"];
          double throttle_value = j[1]["throttle"];

          bike_model.set_coeffs(coeffs);
          Eigen::VectorXd state(6);
          state << 0, 0, 0, v, cte, epsi;
          auto vars = mpc.Solve(state, coeffs);
          steer_value = vars[0];
          throttle_value = vars[1];

          json msgJson;
          // NOTE: Remember to divide by deg2rad(25) before you send the
          // steering value back. Otherwise the values will be in between
          // [-deg2rad(25), deg2rad(25] instead of [-1, 1].
          msgJson["steering_angle"] = steer_value / (deg2rad(25));
          msgJson["throttle"] = throttle_value;

          // Display the MPC predicted trajectory
          vector<double> mpc_x_vals;
          vector<double> mpc_y_vals;

          //.. add (x,y) points to list here, points are in reference to the
          // vehicle's coordinate system
          // the points in the simulator are connected by a Green line

          for (int i = 2; i < vars.size(); i++) {
            if (i % 2 == 0) {
              mpc_x_vals.push_back(vars[i]);
            } else {
              mpc_y_vals.push_back(vars[i]);
            }
          }

          msgJson["mpc_x"] = mpc_x_vals;
          msgJson["mpc_y"] = mpc_y_vals;

          // Display the waypoints/reference line
          vector<double> next_x_vals;
          vector<double> next_y_vals;

          //.. add (x,y) points to list here, points are in reference to the
          // vehicle's coordinate system
          // the points in the simulator are connected by a Yellow line

          for (double i = 0; i < 100; i += 3) {
            next_x_vals.push_back(i);
            next_y_vals.push_back(polyeval(coeffs, i));
          }

          msgJson["next_x"] = next_x_vals;
          msgJson["next_y"] = next_y_vals;

          auto msg = "42[\"steer\"," + msgJson.dump() + "]";
          // std::cout << msg << std::endl;

          // Latency
          // The purpose is to mimic real driving conditions where
          // the car does actuate the commands instantly.
          //
          // Feel free to play around with this value but should be to drive
          // around the track with 100ms latency.
          //
          // NOTE: REMEMBER TO SET THIS TO 100 MILLISECONDS BEFORE
          // SUBMITTING.
          this_thread::sleep_for(chrono::milliseconds(100));
          ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
        }
      } else {
        // Manual driving
        std::string msg = "42[\"manual\",{}]";
        ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
      }
    }
  });

  // We don't need this since we're not using HTTP but if it's removed the
  // program
  // doesn't compile :-(
  h.onHttpRequest([](uWS::HttpResponse *res, uWS::HttpRequest req, char *data,
                     size_t, size_t) {
    const std::string s = "<h1>Hello world!</h1>";
    if (req.getUrl().valueLength == 1) {
      res->end(s.data(), s.length());
    } else {
      // i guess this should be done more gracefully?
      res->end(nullptr, 0);
    }
  });

  h.onConnection([&h](uWS::WebSocket<uWS::SERVER> ws, uWS::HttpRequest req) {
    std::cout << "Connected!!!" << std::endl;
  });

  h.onDisconnection([&h](uWS::WebSocket<uWS::SERVER> ws, int code,
                         char *message, size_t length) {
    ws.close();
    std::cout << "Disconnected" << std::endl;
  });

  int port = 4567;
  if (h.listen(port)) {
    std::cout << "Listening to port " << port << std::endl;
  } else {
    std::cerr << "Failed to listen to port" << std::endl;
    return -1;
  }
  h.run();
}