from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, CMakeDeps, cmake_layout
from conan.tools.files import copy
import os


class Slims3Conan(ConanFile):
    name = "slims3"
    version = "0.1.0"
    license = "MIT"
    author = "Sergey Subbotin"
    url = "https://github.com/ssubbotin/slim-s3"
    homepage = "https://github.com/ssubbotin/slim-s3"
    description = "A slim S3 client for C++17. One dependency: libcurl. No SDK heft."
    topics = ("s3", "aws", "object-storage", "curl", "cpp17")

    settings = "os", "compiler", "build_type", "arch"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
    }

    exports_sources = "CMakeLists.txt", "src/*", "include/*", "LICENSE"

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")

    def layout(self):
        cmake_layout(self)

    def requirements(self):
        self.requires("libcurl/[>=7.61]")

    def generate(self):
        tc = CMakeToolchain(self)
        tc.variables["SLIMS3_BUILD_TESTS"] = False
        tc.generate()
        deps = CMakeDeps(self)
        deps.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        copy(self, "LICENSE", src=self.source_folder, dst=os.path.join(self.package_folder, "licenses"))
        cmake = CMake(self)
        cmake.install()

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "slims3")
        self.cpp_info.set_property("cmake_target_name", "slims3::slims3")
        self.cpp_info.libs = ["slims3"]
