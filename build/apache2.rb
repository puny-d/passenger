#  Phusion Passenger - https://www.phusionpassenger.com/
#  Copyright (c) 2010-2017 Phusion Holding B.V.
#
#  "Passenger", "Phusion Passenger" and "Union Station" are registered
#  trademarks of Phusion Holding B.V.
#
#  Permission is hereby granted, free of charge, to any person obtaining a copy
#  of this software and associated documentation files (the "Software"), to deal
#  in the Software without restriction, including without limitation the rights
#  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
#  copies of the Software, and to permit persons to whom the Software is
#  furnished to do so, subject to the following conditions:
#
#  The above copyright notice and this permission notice shall be included in
#  all copies or substantial portions of the Software.
#
#  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
#  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
#  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
#  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
#  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
#  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
#  THE SOFTWARE.


# apxs totally sucks. We couldn't get it working correctly
# on MacOS X (it had various problems with building universal
# binaries), so we decided to ditch it and build/install the
# Apache module ourselves.
#
# Oh, and libtool sucks too. Do we even need it anymore in 2008?

APACHE2_TARGET = "#{APACHE2_OUTPUT_DIR}mod_passenger.so"
APACHE2_OBJECTS = {
  "#{APACHE2_OUTPUT_DIR}mod_passenger.o" =>
    "src/apache2_module/mod_passenger.cpp",
  "#{APACHE2_OUTPUT_DIR}Config.o" =>
    "src/apache2_module/Config.cpp",
  "#{APACHE2_OUTPUT_DIR}Bucket.o" =>
    "src/apache2_module/Bucket.cpp",
  "#{APACHE2_OUTPUT_DIR}Hooks.o" =>
    "src/apache2_module/Hooks.cpp"
}
APACHE2_AUTOGENERATED_SOURCES = %w(
  src/apache2_module/Config/AutoGeneratedDefinitions.cpp
  src/apache2_module/Config/AutoGeneratedSetterFuncs.cpp
  src/apache2_module/Config/AutoGeneratedHeaderSerialization.cpp
  src/apache2_module/ServerConfig/AutoGeneratedStruct.h
  src/apache2_module/DirConfig/AutoGeneratedCreateFunction.cpp
  src/apache2_module/DirConfig/AutoGeneratedMergeFunction.cpp
  src/apache2_module/DirConfig/AutoGeneratedStruct.h
)

let(:apache2_cxxflags) do
  result = [PlatformInfo.apache2_module_cxxflags]
  result << '-O' if OPTIMIZE
  result.join(' ')
end

# Define compilation tasks for object files.
APACHE2_OBJECTS.each_pair do |object, source|
  define_cxx_object_compilation_task(
    object,
    source,
    lambda { {
      :include_paths => [
        "src/agent",
        *CXX_SUPPORTLIB_INCLUDE_PATHS
      ],
      :flags => apache2_cxxflags
    } }
  )
end

# Define compilation task for the Apache 2 module.
APACHE2_MODULE_BOOST_OXT_LIBRARY, APACHE2_MODULE_BOOST_OXT_LINKARG =
  define_libboost_oxt_task("apache2",
    APACHE2_OUTPUT_DIR + "module_libboost_oxt",
    lambda { PlatformInfo.apache2_module_cxxflags })
APACHE2_MODULE_COMMON_LIBRARIES  = COMMON_LIBRARY.
  only(:base, :base64, 'AppTypes.o').
  set_namespace("apache2").
  set_output_dir(APACHE2_OUTPUT_DIR + "module_libpassenger_common").
  define_tasks(lambda { PlatformInfo.apache2_module_cxxflags }).
  link_objects
dependencies = [
  APACHE2_MODULE_COMMON_LIBRARIES,
  APACHE2_MODULE_BOOST_OXT_LIBRARY,
  APACHE2_OBJECTS.keys
].flatten
file(APACHE2_TARGET => dependencies) do
  PlatformInfo.apache2ctl.nil? and raise "Could not find 'apachectl' or 'apache2ctl'."
  PlatformInfo.httpd.nil?      and raise "Could not find the Apache web server binary."

  sh "mkdir -p #{APACHE2_OUTPUT_DIR}" if !File.directory?(APACHE2_OUTPUT_DIR)
  create_shared_library(APACHE2_TARGET,
    APACHE2_OBJECTS.keys,
    :flags => [
      APACHE2_MODULE_COMMON_LIBRARIES,
      APACHE2_MODULE_BOOST_OXT_LINKARG,
      PlatformInfo.apache2_module_cxx_ldflags,
      PlatformInfo.portability_cxx_ldflags,
      OPTIMIZE ? '-O' : nil,
      USE_ASAN ? "-shared-libasan" : nil
    ].compact
  )
end

desc "Build Apache 2 module"
task :apache2 => [
  APACHE2_TARGET,
  AGENT_TARGET,
  NATIVE_SUPPORT_TARGET
].compact

# Workaround for https://github.com/jimweirich/rake/issues/274
task :_apache2 => :apache2

task :clean => 'apache2:clean'
desc "Clean all compiled Apache 2 files"
task 'apache2:clean' => 'common:clean' do
  files = APACHE2_OBJECTS.keys.dup
  files << APACHE2_TARGET
  sh("rm", "-rf", *files)
end

def create_apache2_auto_generated_source_task(source)
  dependencies = [
    "#{source}.cxxcodebuilder",
    'src/ruby_supportlib/phusion_passenger/apache2/config_options.rb'
  ]
  file(source => dependencies) do
    template = CxxCodeTemplateRenderer.new("#{source}.cxxcodebuilder")
    template.render_to(source)
  end
end

APACHE2_AUTOGENERATED_SOURCES.each do |source|
  create_apache2_auto_generated_source_task(source)
end
