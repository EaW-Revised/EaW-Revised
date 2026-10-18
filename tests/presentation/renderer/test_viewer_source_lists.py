"""The source reader follows the same ordered lists as the viewer target."""
import pathlib
import re
import subprocess

import pytest

import viewer_mode_sources as sources


def test_nested_viewer_lists_keep_order_and_scope(tmp_path, monkeypatch):
    viewer = tmp_path / "apps/viewer"
    (viewer / "src/galactic").mkdir(parents=True)
    (viewer / "CMakeLists.txt").write_text(
        'include("${EAWR_ROOT}/apps/viewer/src/galactic/sources.cmake")\n'
        'include("${EAWR_ROOT}/apps/viewer/src/sources.cmake")\n'
        'add_library(eawr_viewer SHARED ${EAWR_VIEWER_SOURCES})\n')
    (viewer / "src/galactic/sources.cmake").write_text(
        'set(EAWR_VIEWER_GALACTIC_SOURCES "${CMAKE_CURRENT_LIST_DIR}/galaxy.cpp")\n')
    (viewer / "src/sources.cmake").write_text(
        'set(EAWR_VIEWER_SOURCES "${EAWR_BUILD_IDENTITY_CPP}"\n'
        ' "${CMAKE_CURRENT_LIST_DIR}/before.cpp" # ignore_comment.cpp )\n'
        ' ${EAWR_VIEWER_GALACTIC_SOURCES} "${CMAKE_CURRENT_LIST_DIR}/after.cpp")\n'
        'set(EAWR_ROOT_ONLY_SOURCES "${CMAKE_CURRENT_LIST_DIR}/root_only.cpp")\n')
    monkeypatch.setattr(sources, "ROOT", tmp_path)
    text = sources.viewer_build_source()
    assert text.index("before.cpp") < text.index("galaxy.cpp") < text.index("after.cpp")
    assert "root_only.cpp" not in text
    assert "ignore_comment.cpp" not in text
    assert "${EAWR_" not in text.replace("${EAWR_ROOT}", "")


def test_viewer_members_are_unique_and_exist():
    text = sources.viewer_build_source()
    paths = re.findall(r"(?:apps/viewer/src|src/presentation/godot)/[\w/]+\.cpp", text)
    assert len(paths) == len(set(paths))
    assert paths
    assert all((sources.ROOT / pathlib.PurePosixPath(path)).is_file() for path in paths)


def test_unsupported_source_expression_is_rejected(tmp_path, monkeypatch):
    viewer = tmp_path / "apps/viewer"
    (viewer / "src").mkdir(parents=True)
    (viewer / "CMakeLists.txt").write_text(
        'include("${EAWR_ROOT}/apps/viewer/src/sources.cmake")\n'
        'add_library(eawr_viewer SHARED ${EAWR_VIEWER_SOURCES})\n')
    (viewer / "src/sources.cmake").write_text('set(EAWR_VIEWER_SOURCES bare.cpp)\n')
    monkeypatch.setattr(sources, "ROOT", tmp_path)
    with pytest.raises(ValueError, match="Unsupported source expression"):
        sources.viewer_build_source()


def test_viewer_list_requires_galactic_list_first():
    result = subprocess.run(
        ["cmake", "-P", str(sources.SRC / "sources.cmake")],
        capture_output=True, text=True, check=False)
    assert result.returncode != 0
    assert "Include galactic/sources.cmake before" in result.stderr
