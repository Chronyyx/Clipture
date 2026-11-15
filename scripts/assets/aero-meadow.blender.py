"""Builds and renders the ground of the Aero theme's wallpaper in Blender.

A broad hill, high on the left and falling away to the right, covered in
instanced grass blades and lit by Blender's physical sky with a low sun
behind the field, so the blades glow where light comes through them. The
sky itself renders transparent: render-aero-wallpaper.cjs lays this image
over its own sky and cloud.

Run by render-aero-wallpaper.cjs, or by hand:

  blender --background --factory-startup --python scripts/assets/aero-meadow.blender.py -- out.png 2560 1440 [samples]
"""

import math
import sys

import bpy

args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUT = args[0] if args else "meadow-ground.png"
WIDTH = int(args[1]) if len(args) > 1 else 2560
HEIGHT = int(args[2]) if len(args) > 2 else 1440
SAMPLES = int(args[3]) if len(args) > 3 else 96

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene


# --- The hill ---------------------------------------------------------------

def hill_height(x, y):
    """Ground height in metres. The camera looks along +Y from the origin:
    a long rise ahead whose crest sits higher on the left."""
    rise = 9.0 * (1.0 - math.exp(-max(y, 0.0) / 55.0))
    tilt = -0.085 * x * min(max(y, 0.0) / 60.0, 1.0)
    swell = 1.2 * math.exp(-((x + 25.0) ** 2 + (y - 70.0) ** 2) / 1800.0)
    return rise + tilt + swell


bpy.ops.mesh.primitive_grid_add(x_subdivisions=260, y_subdivisions=260, size=1.0)
ground = bpy.context.active_object
ground.name = "Ground"
for vertex in ground.data.vertices:
    # Map the unit grid to 220 m wide by 200 m deep, denser near the camera.
    u, v = vertex.co.x + 0.5, vertex.co.y + 0.5
    x = (u - 0.5) * 220.0
    y = -6.0 + 206.0 * (v ** 1.7)
    vertex.co = (x, y, hill_height(x, y))
bpy.ops.object.shade_smooth()

ground_material = bpy.data.materials.new("Soil")
ground_material.use_nodes = True
nodes = ground_material.node_tree.nodes
links = ground_material.node_tree.links
bsdf = nodes["Principled BSDF"]
noise = nodes.new("ShaderNodeTexNoise")
# Far off the blades thin out, so the ground itself reads as turf there.
noise.inputs["Scale"].default_value = 0.6
noise.inputs["Detail"].default_value = 6.0
ramp = nodes.new("ShaderNodeValToRGB")
ramp.color_ramp.elements[0].color = (0.05, 0.14, 0.012, 1)
ramp.color_ramp.elements[1].color = (0.13, 0.27, 0.025, 1)
links.new(noise.outputs["Fac"], ramp.inputs["Fac"])
links.new(ramp.outputs["Color"], bsdf.inputs["Base Color"])
bsdf.inputs["Roughness"].default_value = 0.95
ground.data.materials.append(ground_material)


# --- A grass blade -----------------------------------------------------------

def make_blade(name, height, width, bend, segments=6):
    """A tapered, curved strip standing on its root at the origin."""
    verts, faces = [], []
    for k in range(segments + 1):
        t = k / segments
        half = width * 0.5 * (1.0 - t) ** 0.8 + 0.0004
        y = bend * height * t * t
        z = height * t * (1.0 - 0.25 * bend * t)
        verts += [(-half, y, z), (half, y, z)]
        if k:
            a = 2 * (k - 1)
            faces.append((a, a + 1, a + 3, a + 2))
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(verts, [], faces)
    mesh.update()
    # The blade's own height, 0 at the root and 1 at the tip, for shading.
    layer = mesh.attributes.new("along", "FLOAT", "POINT")
    for index, vertex in enumerate(mesh.vertices):
        layer.data[index].value = vertex.co.z / height
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    for poly in mesh.polygons:
        poly.use_smooth = True
    return obj


blades = [
    make_blade("BladeA", 0.30, 0.010, 0.35),
    make_blade("BladeB", 0.42, 0.012, 0.55),
    make_blade("BladeC", 0.22, 0.008, 0.20),
    make_blade("BladeD", 0.36, 0.014, 0.75),
]
blade_collection = bpy.data.collections.new("Blades")
bpy.context.scene.collection.children.link(blade_collection)
for blade in blades:
    for owner in list(blade.users_collection):
        owner.objects.unlink(blade)
    blade_collection.objects.link(blade)
blade_collection.hide_render = True

grass_material = bpy.data.materials.new("Grass")
grass_material.use_nodes = True
nodes = grass_material.node_tree.nodes
links = grass_material.node_tree.links
nodes.clear()
output = nodes.new("ShaderNodeOutputMaterial")
along = nodes.new("ShaderNodeAttribute")
along.attribute_name = "along"
info = nodes.new("ShaderNodeObjectInfo")
# Per-blade hue: a spread of greens, a few going straw.
hue = nodes.new("ShaderNodeValToRGB")
hue.color_ramp.elements[0].color = (0.020, 0.13, 0.004, 1)
hue.color_ramp.elements[1].color = (0.10, 0.34, 0.008, 1)
mid = hue.color_ramp.elements.new(0.55)
mid.color = (0.045, 0.24, 0.006, 1)
straw = hue.color_ramp.elements.new(0.94)
straw.color = (0.20, 0.34, 0.03, 1)
links.new(info.outputs["Random"], hue.inputs["Fac"])
# Root to tip: dark at the root, the blade's own colour above.
shade = nodes.new("ShaderNodeValToRGB")
shade.color_ramp.elements[0].color = (0.18, 0.22, 0.12, 1)
shade.color_ramp.elements[1].color = (1.0, 1.0, 1.0, 1)
shade.color_ramp.elements[1].position = 0.6
links.new(along.outputs["Fac"], shade.inputs["Fac"])
tint = nodes.new("ShaderNodeMix")
tint.data_type = "RGBA"
tint.blend_type = "MULTIPLY"
tint.inputs["Factor"].default_value = 1.0
links.new(hue.outputs["Color"], tint.inputs["A"])
links.new(shade.outputs["Color"], tint.inputs["B"])
surface = nodes.new("ShaderNodeBsdfPrincipled")
surface.inputs["Roughness"].default_value = 0.7
surface.inputs["Specular IOR Level"].default_value = 0.12
links.new(tint.outputs["Result"], surface.inputs["Base Color"])
# Light through the blade: a yellow-green glow when the sun is behind it.
glow = nodes.new("ShaderNodeBsdfTranslucent")
glow_colour = nodes.new("ShaderNodeMix")
glow_colour.data_type = "RGBA"
glow_colour.blend_type = "MULTIPLY"
glow_colour.inputs["Factor"].default_value = 1.0
glow_colour.inputs["B"].default_value = (1.0, 1.2, 0.45, 1)
links.new(tint.outputs["Result"], glow_colour.inputs["A"])
links.new(glow_colour.outputs["Result"], glow.inputs["Color"])
mix = nodes.new("ShaderNodeMixShader")
mix.inputs["Fac"].default_value = 0.35
links.new(surface.outputs["BSDF"], mix.inputs[1])
links.new(glow.outputs["BSDF"], mix.inputs[2])
links.new(mix.outputs["Shader"], output.inputs["Surface"])
for blade in blades:
    blade.data.materials.append(grass_material)


# --- Scattering the blades ---------------------------------------------------

modifier = ground.modifiers.new("Grass", "NODES")
tree = bpy.data.node_groups.new("Grass", "GeometryNodeTree")
modifier.node_group = tree
tree.interface.new_socket("Geometry", in_out="INPUT", socket_type="NodeSocketGeometry")
tree.interface.new_socket("Geometry", in_out="OUTPUT", socket_type="NodeSocketGeometry")
g = tree.nodes
gl = tree.links
group_in = g.new("NodeGroupInput")
group_out = g.new("NodeGroupOutput")

# Density: thick near the camera, thinning with distance, nothing behind it
# or outside the view.
position = g.new("GeometryNodeInputPosition")
separate = g.new("ShaderNodeSeparateXYZ")
gl.new(position.outputs["Position"], separate.inputs["Vector"])
distance = g.new("ShaderNodeVectorMath")
distance.operation = "LENGTH"
gl.new(position.outputs["Position"], distance.inputs[0])
falloff = g.new("ShaderNodeMath")
falloff.operation = "DIVIDE"
falloff.inputs[0].default_value = 12.0
gl.new(distance.outputs["Value"], falloff.inputs[1])
squared = g.new("ShaderNodeMath")
squared.operation = "POWER"
squared.inputs[1].default_value = 2.2
gl.new(falloff.outputs["Value"], squared.inputs[0])
clamp = g.new("ShaderNodeClamp")
clamp.inputs["Min"].default_value = 0.0
clamp.inputs["Max"].default_value = 1.0
gl.new(squared.outputs["Value"], clamp.inputs["Value"])
# In view: |x| below the depth (a 90 degree wedge) plus a margin.
abs_x = g.new("ShaderNodeMath")
abs_x.operation = "ABSOLUTE"
gl.new(separate.outputs["X"], abs_x.inputs[0])
margin = g.new("ShaderNodeMath")
margin.operation = "ADD"
margin.inputs[1].default_value = 4.0
gl.new(separate.outputs["Y"], margin.inputs[0])
inside = g.new("ShaderNodeMath")
inside.operation = "LESS_THAN"
gl.new(abs_x.outputs["Value"], inside.inputs[0])
gl.new(margin.outputs["Value"], inside.inputs[1])
factor = g.new("ShaderNodeMath")
factor.operation = "MULTIPLY"
gl.new(clamp.outputs["Result"], factor.inputs[0])
gl.new(inside.outputs["Value"], factor.inputs[1])

distribute = g.new("GeometryNodeDistributePointsOnFaces")
distribute.distribute_method = "RANDOM"
distribute.inputs["Seed"].default_value = 7
gl.new(group_in.outputs["Geometry"], distribute.inputs["Mesh"])
# Random distribution takes its density as a field: blades per square metre.
density = g.new("ShaderNodeMath")
density.operation = "MULTIPLY"
density.inputs[1].default_value = 3000.0
gl.new(factor.outputs["Value"], density.inputs[0])
gl.new(density.outputs["Value"], distribute.inputs["Density"])

collection_info = g.new("GeometryNodeCollectionInfo")
collection_info.inputs["Collection"].default_value = blade_collection
collection_info.inputs["Separate Children"].default_value = True
collection_info.inputs["Reset Children"].default_value = True

# Each blade turned at random, leaning a little downwind, and sized.
rotation = g.new("FunctionNodeRandomValue")
rotation.data_type = "FLOAT_VECTOR"
rotation.inputs["Min"].default_value = (-0.12, -0.12, 0.0)
rotation.inputs["Max"].default_value = (0.12, 0.2, math.tau)
rotation.inputs["Seed"].default_value = 3
scale = g.new("FunctionNodeRandomValue")
scale.data_type = "FLOAT"
scale.inputs["Min"].default_value = 0.6
scale.inputs["Max"].default_value = 1.5
scale.inputs["Seed"].default_value = 5
pick = g.new("FunctionNodeRandomValue")
pick.data_type = "BOOLEAN"
pick.inputs["Probability"].default_value = 1.0

instance = g.new("GeometryNodeInstanceOnPoints")
instance.inputs["Pick Instance"].default_value = True
gl.new(distribute.outputs["Points"], instance.inputs["Points"])
gl.new(collection_info.outputs["Instances"], instance.inputs["Instance"])
gl.new(rotation.outputs[0], instance.inputs["Rotation"])
gl.new(scale.outputs[1], instance.inputs["Scale"])

join = g.new("GeometryNodeJoinGeometry")
gl.new(group_in.outputs["Geometry"], join.inputs["Geometry"])
gl.new(instance.outputs["Instances"], join.inputs["Geometry"])
gl.new(join.outputs["Geometry"], group_out.inputs["Geometry"])


# --- Light and camera --------------------------------------------------------

world = bpy.data.worlds.new("Sky")
scene.world = world
world.use_nodes = True
sky = world.node_tree.nodes.new("ShaderNodeTexSky")
sky.sky_type = "NISHITA"
sky.sun_elevation = math.radians(28.0)
sky.sun_rotation = math.radians(160.0)
sky.air_density = 1.0
sky.dust_density = 0.6
background = world.node_tree.nodes["Background"]
background.inputs["Strength"].default_value = 0.35
world.node_tree.links.new(sky.outputs["Color"], background.inputs["Color"])

sun_data = bpy.data.lights.new("Sun", "SUN")
sun_data.energy = 4.5
sun_data.angle = math.radians(1.5)
sun_data.color = (1.0, 0.96, 0.86)
sun = bpy.data.objects.new("Sun", sun_data)
scene.collection.objects.link(sun)
# High behind the field and to the right, so blades are backlit.
sun.rotation_euler = (math.radians(62.0), 0.0, math.radians(160.0))

camera_data = bpy.data.cameras.new("Camera")
camera_data.lens = 28.0
camera_data.dof.use_dof = True
camera_data.dof.focus_distance = 14.0
camera_data.dof.aperture_fstop = 4.0
camera = bpy.data.objects.new("Camera", camera_data)
scene.collection.objects.link(camera)
camera.location = (0.0, -2.0, 1.3)
camera.rotation_euler = (math.radians(99.5), 0.0, 0.0)
scene.camera = camera


# --- Render -----------------------------------------------------------------

scene.render.engine = "CYCLES"
scene.cycles.samples = SAMPLES
scene.cycles.use_denoising = True
scene.render.film_transparent = True
scene.render.resolution_x = WIDTH
scene.render.resolution_y = HEIGHT
scene.render.resolution_percentage = 100
scene.render.image_settings.file_format = "PNG"
scene.render.image_settings.color_mode = "RGBA"
scene.view_settings.view_transform = "AgX"
scene.view_settings.look = "AgX - Punchy"
scene.view_settings.exposure = -0.4

preferences = bpy.context.preferences.addons["cycles"].preferences
for backend in ("OPTIX", "CUDA"):
    try:
        preferences.compute_device_type = backend
        preferences.get_devices()
        if any(device.type == backend for device in preferences.devices):
            for device in preferences.devices:
                device.use = device.type == backend
            scene.cycles.device = "GPU"
            break
    except TypeError:
        continue

scene.render.filepath = OUT
bpy.ops.render.render(write_still=True)
print(f"Rendered {OUT} ({WIDTH}x{HEIGHT}, {SAMPLES} samples)")
