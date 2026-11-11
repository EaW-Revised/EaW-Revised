extends SceneTree

func _initialize() -> void:
	var results: Array = []
	var passed := DisplayServer.get_name() == "headless"
	for skinned in [false, true]:
		var payload: Dictionary = ClassDB.class_call_static("ViewerHost", "prewarm_mesh_layout", skinned)
		var arrays: Array = payload["arrays"]
		var format: int = payload["format"]
		var vertices: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		var custom = arrays[Mesh.ARRAY_CUSTOM0]
		var custom_format := (format >> Mesh.ARRAY_FORMAT_CUSTOM0_SHIFT) & Mesh.ARRAY_FORMAT_CUSTOM_MASK
		var type_ok := typeof(custom) == TYPE_PACKED_FLOAT32_ARRAY
		var count_ok: bool = custom.size() == vertices.size() * 3
		var skin_ok: bool = (arrays[Mesh.ARRAY_BONES] != null) == skinned
		if skinned:
			skin_ok = skin_ok and arrays[Mesh.ARRAY_BONES].size() == vertices.size() * 4
			skin_ok = skin_ok and arrays[Mesh.ARRAY_WEIGHTS].size() == vertices.size() * 4
		else:
			skin_ok = skin_ok and arrays[Mesh.ARRAY_WEIGHTS] == null
		# Always run the actual engine validator, including when the type is
		# wrong, so a reverted constructor reproduces its real type diagnostic.
		var mesh := ArrayMesh.new()
		mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays, [], {}, format)
		var count := mesh.get_surface_count()
		var accepted := type_ok and count_ok and skin_ok and custom_format == Mesh.ARRAY_CUSTOM_RGB_FLOAT and count == 1
		passed = passed and accepted
		results.append({"skinned": skinned, "slot": Mesh.ARRAY_CUSTOM0,
			"variant_type": type_string(typeof(custom)), "elements": custom.size(),
			"vertices": vertices.size(), "custom_format": custom_format,
			"surface_count": count, "skin_layout_correct": skin_ok, "passed": accepted})
	print("P9_PREWARM_BUILDER " + JSON.stringify({"headless": DisplayServer.get_name() == "headless",
		"production_builder": true, "cases": results, "passed": passed}))
	quit(0 if passed else 1)
