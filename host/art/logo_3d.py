# Renders the launcher logo in 3D with Blender, after the game's box logo: tall
# extruded "BAD FUR DAY." letters with a textured orange face, a polished silver rim
# and dark brushed-metal sides, and a glossy red tube script for "Conker's" and
# "Reloaded".
#   blender -b -P logo_3d.py -- <out.png>
# Fonts: Poppins Black (SIL OFL 1.1), Coming Soon (Apache License 2.0).
import math
import os
import sys

import bpy
from mathutils import Vector

ART = os.path.dirname(os.path.abspath(__file__))
OUT = sys.argv[sys.argv.index("--") + 1]

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
block_font = bpy.data.fonts.load(os.path.join(ART, "Poppins-Black.ttf"))
script_font = bpy.data.fonts.load(os.path.join(ART, "ComingSoon-Regular.ttf"))

DEPTH = 0.32        # half the letters' thickness (text extrudes both ways)
RIM = 0.03          # width of the silver rim around each orange face
FATTEN = 0.010      # thickens Poppins' strokes towards the box logo's
LETTER_SCALE = (0.95, 1.18, 1.0)


def material(name, color, metallic=0.0, roughness=0.5, coat=0.0, bump=0.0, aniso=0.0):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    p = nt.nodes["Principled BSDF"]
    p.inputs["Base Color"].default_value = (*color, 1)
    p.inputs["Metallic"].default_value = metallic
    p.inputs["Roughness"].default_value = roughness
    p.inputs["Coat Weight"].default_value = coat
    p.inputs["Anisotropic"].default_value = aniso
    if bump > 0:
        noise = nt.nodes.new("ShaderNodeTexNoise")
        noise.inputs["Scale"].default_value = 900.0
        noise.inputs["Detail"].default_value = 6.0
        b = nt.nodes.new("ShaderNodeBump")
        b.inputs["Strength"].default_value = bump
        nt.links.new(noise.outputs["Fac"], b.inputs["Height"])
        nt.links.new(b.outputs["Normal"], p.inputs["Normal"])
    return m


FELT = material("felt", (0.72, 0.16, 0.01), roughness=0.85, bump=0.2)
SILVER = material("silver", (0.92, 0.92, 0.94), metallic=1.0, roughness=0.12)
STEEL = material("steel", (0.55, 0.56, 0.60), metallic=1.0, roughness=0.32, aniso=0.7)
RED = material("red", (0.75, 0.01, 0.015), roughness=0.22, coat=0.8)


def text_object(name, body, font, size, extrude, bevel, offset=0.0, spacing=1.0, resolution=4):
    cu = bpy.data.curves.new(name, "FONT")
    cu.body = body
    cu.font = font
    cu.size = size
    cu.extrude = extrude
    cu.bevel_depth = bevel
    cu.bevel_resolution = resolution
    cu.offset = offset
    cu.space_character = spacing
    ob = bpy.data.objects.new(name, cu)
    scene.collection.objects.link(ob)
    return ob


def to_mesh(ob):
    bpy.ops.object.select_all(action="DESELECT")
    bpy.context.view_layer.objects.active = ob
    ob.select_set(True)
    bpy.ops.object.convert(target="MESH")
    return ob


def block_row(text, x, y, spacing):
    """Returns the row's (body, face)."""
    # Body: the whole letter in metal, bright silver where it faces the camera.
    body = text_object("body_" + text, text, block_font, 1.0, DEPTH, 0.012, offset=FATTEN, spacing=spacing)
    body.location = (x, y, 0)
    body.scale = LETTER_SCALE
    to_mesh(body)
    body.data.materials.append(SILVER)
    body.data.materials.append(STEEL)
    for poly in body.data.polygons:
        poly.material_index = 0 if poly.normal.z > 0.5 else 1
    # Face: the same letters shrunk by the rim, as a thin orange plate on the front.
    face = text_object("face_" + text, text, block_font, 1.0, 0.004, 0.0, offset=FATTEN - RIM, spacing=spacing)
    face.location = (x, y, DEPTH + 0.02)
    face.scale = LETTER_SCALE
    face.data.materials.append(FELT)
    return body, face


def script(text, loc, rot_deg, size):
    ob = text_object("script_" + text, text, script_font, size, 0.012, 0.017, resolution=6)
    ob.location = loc
    ob.rotation_euler = (0, 0, math.radians(rot_deg))
    ob.data.materials.append(RED)


# Rows stacked by their measured height, touching like the box logo's.
y = 0.0
rows = []
for text, x in (("BAD", 0.0), ("FUR", 0.03), ("DAY.", 0.0)):
    body, face = block_row(text, x, y, 0.86)
    bpy.context.view_layer.update()
    height = body.dimensions.y - 2 * 0.012
    rows.append((body, face, height))
top = rows[0][2]
y = 0.0
for body, face, height in rows:
    body.location.y = face.location.y = y
    y -= height * 1.02
script("Conker's", (-0.20, top - 0.12, DEPTH + 0.10), 9, 0.50)
script("Reloaded", (0.40, y + 0.17, DEPTH + 0.10), 5, 0.42)
logo_center = Vector(((rows[0][0].dimensions.x) / 2 + 0.1, (top + 0.45 + y - 0.2) / 2, 0))

# Lights: a big soft key from the upper left, a cooler fill from the right, and a
# bright strip above for the chrome to reflect.
def area(name, loc, size, energy, color=(1, 1, 1)):
    light = bpy.data.lights.new(name, "AREA")
    light.size = size
    light.energy = energy
    light.color = color
    ob = bpy.data.objects.new(name, light)
    ob.location = loc
    scene.collection.objects.link(ob)
    direction = Vector((1.4, -1.2, 0)) - Vector(loc)
    ob.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()


area("key", (-4, 4, 7), 6, 800)
area("left", (-6, -1, 2), 4, 500)
area("fill", (7, -2, 5), 5, 500, (0.85, 0.9, 1.0))
area("rim", (2, 6, 2), 3, 400)

world = bpy.data.worlds.new("world")
scene.world = world
world.use_nodes = True
wn = world.node_tree
coords = wn.nodes.new("ShaderNodeTexCoord")
sep = wn.nodes.new("ShaderNodeSeparateXYZ")
ramp = wn.nodes.new("ShaderNodeValToRGB")
ramp.color_ramp.elements[0].position = 0.45
ramp.color_ramp.elements[0].color = (0.02, 0.02, 0.025, 1)
ramp.color_ramp.elements[1].position = 0.75
ramp.color_ramp.elements[1].color = (0.9, 0.9, 0.95, 1)
wn.links.new(coords.outputs["Generated"], sep.inputs["Vector"])
wn.links.new(sep.outputs["Z"], ramp.inputs["Fac"])
wn.links.new(ramp.outputs["Color"], wn.nodes["Background"].inputs["Color"])
wn.nodes["Background"].inputs["Strength"].default_value = 0.6

cam_data = bpy.data.cameras.new("cam")
cam_data.lens = 64
cam = bpy.data.objects.new("cam", cam_data)
scene.collection.objects.link(cam)
# Looking down -Z, up +Y, from up and to the left of the logo, so the letters' metal
# sides show on their left and top as on the box.
target = logo_center
offset = Vector((-2.2, 1.8, 4.6))
cam.location = target + offset
cam.rotation_euler = (-math.atan2(offset.y, offset.z), -math.atan2(-offset.x, offset.z), 0)
scene.camera = cam

scene.render.engine = "CYCLES"
scene.render.film_transparent = True
scene.render.resolution_x = 1500
scene.render.resolution_y = 1600
scene.render.image_settings.file_format = "PNG"
scene.render.image_settings.color_mode = "RGBA"
scene.render.filepath = OUT
scene.view_settings.view_transform = "Standard"
scene.cycles.samples = 128
scene.cycles.use_denoising = True
try:
    prefs = bpy.context.preferences.addons["cycles"].preferences
    for kind in ("OPTIX", "CUDA"):
        try:
            prefs.compute_device_type = kind
            prefs.get_devices()
            if any(d.type == kind for d in prefs.devices):
                for d in prefs.devices:
                    d.use = d.type == kind
                scene.cycles.device = "GPU"
                print("render device:", kind)
                break
        except TypeError:
            pass
except Exception as e:
    print("GPU setup failed, using the CPU:", e)

bpy.ops.render.render(write_still=True)
print("wrote", OUT)
