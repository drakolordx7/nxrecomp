// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode (S48): the mouse and the right stick turn the morph ball camera around the ball.
//
// How the game's ball camera works (CBallCameraMP1, read in IDA; the same as the GameCube CBallCamera): it is a leash. Every Think starts from
// where the camera is now, takes the flat direction from the ball to the camera, and keeps the camera on that side of the ball at the tweak's
// distance and height, pulled by springs and pushed out of walls by its colliders. Nothing in it reads the stick (CBallCameraMP1::ProcessInput only
// reads the chase-camera button), so Remastered gives the right stick no job in the ball.
//
// So the turn goes where the game keeps it:
//   * yaw: before the game's Think, the camera is turned around the ball by the turn collected since the last Think, through the game's own
//     CBallCameraMP1::TeleportCamera (it moves the transform, the damped position, the three collider rings and the collision actor). The game's
//     Think then carries on from there with all its own collision handling, and the camera stays on its new side. Because the game's camera itself
//     has turned, the ball rolls relative to the camera you see (the controls follow the view).
//   * pitch: the game puts the camera's height back to its tweak on every Think, so the vertical turn is a view offset instead: after the Think
//     the camera is turned up or down around the ball, and the game's own transform is put back before the next Think. The offset is kept in a
//     range of camera heights above the ball (not under the floor, not straight overhead).
// Only while the camera is the game's free ball camera (state default, chase or boost; behaviour default or freeze-look): fixed, path and spindle
// cameras of a room and the morph transitions belong to the game, and a turn made then is dropped.
#pragma once
#include <cstdint>

#include "play_camlook.h"

namespace nxrt::play {

struct BallCamLayout {
    uint64_t fn_teleport = 0xcf6d84;   // CBallCameraMP1::TeleportCamera(CTransform4f const&, CStateManager&): x0 camera, x1 transform, x2 state manager
    uint32_t state = 0x510;            // EBallCameraState: 0 default, 1 one, 2 chase, 3 boost, 4 to ball, 5 from ball (Think 0xcff7ec, ProcessInput 0xcffe34)
    uint32_t behaviour = 0x28c;        // EBallCameraBehaviour: 0 default, 1 freeze look, 2 hint ball-to-cam, 3 init position, 4 and 5 fixed, 6 path desired
                                       // position, 7 path, 8 spindle (the switch in Think)
    float ball_radius = 0.7f;          // the pivot is the player's position (the bottom of the ball) plus this, up
};

// True when the game's ball camera is free (the turn may move it).
bool ballcam_free(uint32_t state, uint32_t behaviour);

// The heading (radians, + = turned left, 0 = looking along +Z) and the pitch (+ = looking up) of a camera transform (row-major 3x4, forward = column 2).
void camera_heading_pitch(const float xf[12], double* heading, double* pitch);

// The elevation of the camera above the pivot, radians (0 = level with it, + = above).
double camera_elevation(const float xf[12], const float pivot[3]);

// The pitch offset (+ = look up, which lowers the camera) that can be applied to a camera at elevation `elev` without leaving
// [min_elev, max_elev]: `want` clamped to [elev - max_elev, elev - min_elev].
double clamp_ball_pitch(double want, double elev, double min_elev, double max_elev);

// What the input asks of the ball camera: a turn collected until the next Think (yaw, radians, + = view turns left) and a held pitch offset.
class BallOrbit {
public:
    // Mouse counts (the same signs as the aim: right turns right, up looks up; invert flags flip them).
    void add_mouse(double dx, double dy, const LookParams& p);
    // Radians (a stick: rate times the frame time). dyaw > 0 turns the view left, dpitch > 0 looks up.
    void add_angles(double dyaw, double dpitch, const LookParams& p);
    // The yaw collected since the last call (Think takes it).
    double take_yaw() { const double y = yaw_; yaw_ = 0; return y; }
    double pending_yaw() const { return yaw_; }
    double pitch() const { return pitch_; }
    void set_pitch(double p) { pitch_ = p; }
    void reset() { yaw_ = pitch_ = 0; }
private:
    double yaw_ = 0, pitch_ = 0;
};

// The limits of the camera's elevation above the ball for the pitch offset.
constexpr double kBallMinElevation = 5.0 * 3.14159265358979323846 / 180.0;
constexpr double kBallMaxElevation = 80.0 * 3.14159265358979323846 / 180.0;

}  // namespace nxrt::play
