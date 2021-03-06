// systemtap compile-server server backends.
// Copyright (C) 2017 Red Hat Inc.
//
// This file is part of systemtap, and is free software.  You can
// redistribute it and/or modify it under the terms of the GNU General
// Public License (GPL); either version 2, or (at your option) any
// later version.

#include "backends.h"
#include <iostream>
#include <fstream>
#include "../util.h"

extern "C" {
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <spawn.h>
#include <string.h>
#include <glob.h>
#include <sys/utsname.h>
}

using namespace std;


class default_backend : public backend_base
{
public:
    
    bool can_generate_module(const client_request_data *) {
	return true;
    }
    int generate_module(const client_request_data *crd,
			const vector<string> &argv,
			const string &tmp_dir,
			const string &uuid,
			const string &stdout_path,
			const string &stderr_path);
};

int
default_backend::generate_module(const client_request_data *crd,
				 const vector<string> &,
				 const string &,
				 const string &,
				 const string &stdout_path,
				 const string &stderr_path)
{
    ofstream stdout_stream, stderr_stream;

    // Create an empty stdout file.
    stdout_stream.open(stdout_path);
    if (stdout_stream.is_open()) {
	stdout_stream.close();
    }

    // Create an stderr file with an error message.
    stderr_stream.open(stderr_path);
    if (stderr_stream.is_open()) {
	stderr_stream << "Error: the server cannot create a module for kernel "
		      << crd->kver << ", architecture " << crd->arch
		      << ", distro " << crd->distro_name << endl;
	stderr_stream.close();
    }
    return 1;
}


class local_backend : public backend_base
{
public:
    local_backend();

    bool can_generate_module(const client_request_data *crd);
    int generate_module(const client_request_data *crd,
			const vector<string> &argv,
			const string &tmp_dir,
			const string &uuid,
			const string &stdout_path,
			const string &stderr_path);

private:
    // <kernel version, build tree path>
    map<string, string> supported_kernels;

    string distro_name;

    // The current architecture.
    string arch;
};


local_backend::local_backend()
{
    glob_t globber;
    string pattern = "/lib/modules/*/build";
    int rc = glob(pattern.c_str(), GLOB_ERR, NULL, &globber);

    if (rc) {
	// We weren't able to find any kernel build trees. This isn't
	// a fatal error, since one of the other backends might be
	// able to satisfy requests.
	//
	// FIXME: By reading the directory here, we'll only see
	// kernel build trees installed at startup. If a kernel build
	// tree gets installed after startup, we won't see it.
	return;
    }
    for (unsigned int i = 0; i < globber.gl_pathc; i++) {
	string path = globber.gl_pathv[i];

	supported_kernels.insert({kernel_release_from_build_tree(path), path});
    }
    globfree(&globber);

    // Notice we don't error if we can't get the distro name. This
    // isn't a fatal error, since other backends might be able to
    // handle this request.
    vector<string> info;
    get_distro_info(info);
    if (! info.empty()) {
	distro_name = info[0];
	transform(distro_name.begin(), distro_name.end(), distro_name.begin(), ::tolower);
    }

    // Get the current arch name.
    struct utsname buf;
    (void)uname(&buf);
    arch = buf.machine;
}

bool
local_backend::can_generate_module(const client_request_data *crd)
{
    // See if we support the kernel/arch/distro combination.
    if (supported_kernels.count(crd->kver) == 1 && arch == crd->arch
	&& distro_name == crd->distro_name) {
	return true;
    }

    return false;
}

int
local_backend:: generate_module(const client_request_data *,
				const vector<string> &argv,
				const string &,
				const string &,
				const string &stdout_path,
				const string &stderr_path)
{
    // Handle capturing stdout and stderr (along with using /dev/null
    // for stdin).
    posix_spawn_file_actions_t actions;
    int rc = posix_spawn_file_actions_init(&actions);
    if (rc == 0) {
	rc = posix_spawn_file_actions_addopen(&actions, 0, "/dev/null",
					      O_RDONLY, S_IRWXU);
    }
    if (rc == 0) {
	rc = posix_spawn_file_actions_addopen(&actions, 1,
					      stdout_path.c_str(),
					      O_WRONLY|O_CREAT,
					      S_IRWXU);
    }
    if (rc == 0) {
	rc = posix_spawn_file_actions_addopen(&actions, 2,
					      stderr_path.c_str(),
					      O_WRONLY|O_CREAT,
					      S_IRWXU);
    }
    if (rc != 0) {
	clog << "posix_spawn_file_actions failed: " << strerror(errno)
	     << endl;
	return rc;
    }

    // Kick off stap.
    pid_t pid;
    pid = stap_spawn(2, argv, &actions);
    clog << "spawn returned " << pid << endl;

    // If stap_spawn() failed, no need to wait.
    if (pid == -1) {
	rc = errno;
	clog << "Error in spawn: " << strerror(errno) << endl;
	(void)posix_spawn_file_actions_destroy(&actions);
	return rc;
    }

    // Wait on the spawned process to finish.
    rc = stap_waitpid(0, pid);
    if (rc < 0) {			// stap_waitpid() failed
	clog << "waitpid failed: " << strerror(errno) << endl;
	(void)posix_spawn_file_actions_destroy(&actions);
	return rc;
    }

    clog << "Spawned process returned " << rc << endl;
    (void)posix_spawn_file_actions_destroy(&actions);

    return rc;
}


class docker_backend : public backend_base
{
public:
    docker_backend();

    bool can_generate_module(const struct client_request_data *crd);
    int generate_module(const client_request_data *crd,
			const vector<string> &argv,
			const string &tmp_dir,
			const string &uuid,
			const string &stdout_path,
			const string &stderr_path);

private:
    // The docker executable path.
    string docker_path;

    // The docker data directory.
    string datadir;
    
    // List of docker data filenames. <distro name, path>
    map<string, string> data_files;

    // The current architecture.
    string arch;

    // The script path that builds a docker container.
    string docker_build_container_script_path;
};


docker_backend::docker_backend()
{
    try {
	docker_path = find_executable("docker");
	// If find_executable() can't find the path, it returns the
	// name you passed it.
	if (docker_path == "docker")
	    docker_path.clear();
    }
    catch (...) {
	// It really isn't an error for the system to not have the
	// "docker" executable. We'll just disallow builds using the
	// docker backend (down in
	// docker_backend::can_generate_module()).
	docker_path.clear();
    }
    
    docker_build_container_script_path = string(PKGLIBDIR)
	+ "/httpd/docker/stap_build_docker_container.py";

    datadir = string(PKGDATADIR) + "/httpd/docker";

    glob_t globber;
    string pattern = datadir + "/*.json";
    int rc = glob(pattern.c_str(), GLOB_ERR, NULL, &globber);
    if (rc) {
	// We weren't able to find any JSON docker data files. This
	// isn't a fatal error, since one of the other backends might
	// be able to satisfy requests.
	//
	// FIXME: By reading the directory here, we'll only see distro
	// json files installed at startup. If one gets installed
	// after startup, we won't see it.
	return;
    }
    for (unsigned int i = 0; i < globber.gl_pathc; i++) {
	string path = globber.gl_pathv[i];
	
	size_t found = path.find_last_of("/");
	if (found != string::npos) {
	    // First, get the file basename ("FOO.json").
	    string filename = path.substr(found + 1);

	    // Now, chop off the .json extension.
	    size_t found = filename.find_last_of(".");
	    if (found != string::npos) {
		// Notice we're lowercasing the distro name to make
		// things simpler.
		string distro = filename.substr(0, found);
		transform(distro.begin(), distro.end(), distro.begin(),
			  ::tolower);
		data_files.insert({distro, path});
	    }
	}
    }
    globfree(&globber);

    // Get the current arch name.
    struct utsname buf;
    (void)uname(&buf);
    arch = buf.machine;
}

bool
docker_backend::can_generate_module(const client_request_data *crd)
{
    // If we don't have a docker executable, we're done.
    if (docker_path.empty())
	return false;

    // We have to see if we have a JSON data file for that distro and
    // the arches match.
    if (data_files.count(crd->distro_name) == 1 && arch == crd->arch) {
	return true;
    }

    return false;
}

int
docker_backend::generate_module(const client_request_data *crd,
				const vector<string> &argv,
				const string &tmp_dir,
				const string &uuid,
				const string &stdout_path,
				const string &stderr_path)
{
    // FIXME: Here we'll need to generate a docker file, run docker to
    // create the container (and get all the right requirements
    // installed), copy any files over, then finally run "docker exec"
    // to actually run stap.

    // Handle capturing stdout and stderr (along with using /dev/null
    // for stdin).
    posix_spawn_file_actions_t actions;
    string docker_stdout_path = string(tmp_dir) + "/docker_stdout";
    string docker_stderr_path = string(tmp_dir) + "/docker_stderr";
    int rc = posix_spawn_file_actions_init(&actions);
    if (rc == 0) {
	rc = posix_spawn_file_actions_addopen(&actions, 0, "/dev/null",
					      O_RDONLY, S_IRWXU);
    }
    if (rc == 0) {
	rc = posix_spawn_file_actions_addopen(&actions, 1,
					      docker_stdout_path.c_str(),
					      O_WRONLY|O_CREAT|O_EXCL,
					      S_IRWXU);
    }
    if (rc == 0) {
	rc = posix_spawn_file_actions_addopen(&actions, 2,
					      docker_stderr_path.c_str(),
					      O_WRONLY|O_CREAT|O_EXCL,
					      S_IRWXU);
    }
    if (rc != 0) {
	clog << "posix_spawn_file_actions failed: " << strerror(errno)
	     << endl;
	return rc;
    }

    // Grab a JSON representation of the client_request_data, and
    // write it to a file (so the script that generates the docker
    // file(s) knows what it is supposed to be doing).
    string build_data_path = string(tmp_dir) + "/build_data.json";
    struct json_object *root = crd->get_json_object();
    clog << "JSON data: " << json_object_to_json_string(root) << endl;
    ofstream build_data_file;
    build_data_file.open(build_data_path, ios::out);
    build_data_file << json_object_to_json_string(root);
    build_data_file.close();
    json_object_put(root);

    // Kick off building the docker container. Note we're using the
    // UUID as the docker container name. This keeps us from trying to
    // build multiple containers with the same name at the same time.
    vector<string> docker_args;
    docker_args.push_back("python");
    docker_args.push_back(docker_build_container_script_path);
    docker_args.push_back("--distro-file");
    docker_args.push_back(data_files[crd->distro_name]);
    docker_args.push_back("--build-file");
    docker_args.push_back(build_data_path);
    docker_args.push_back("--data-dir");
    docker_args.push_back(datadir);
    docker_args.push_back(uuid);
    pid_t pid = stap_spawn(2, docker_args, &actions);
    clog << "spawn returned " << pid << endl;

    // If stap_spawn() failed, no need to wait.
    if (pid == -1) {
	rc = errno;
	clog << "Error in spawn: " << strerror(errno) << endl;
	(void)posix_spawn_file_actions_destroy(&actions);
	return rc;
    }

    // Wait on the spawned process to finish.
    rc = stap_waitpid(0, pid);
    if (rc < 0) {			// stap_waitpid() failed
	clog << "waitpid failed: " << strerror(errno) << endl;
	(void)posix_spawn_file_actions_destroy(&actions);
	return rc;
    }

    clog << "Spawned process returned " << rc << endl;
    (void)posix_spawn_file_actions_destroy(&actions);

    // If the client requested it, append the docker build output to
    // the client's stdout/stderr files.
    if (crd->verbose >= 3) {
	ifstream docker_file;
	ofstream client_file;

	// Copy over stdout data.
	docker_file.open(docker_stdout_path, ios::in);
	client_file.open(stdout_path, ios::out|ios::app);
	client_file << docker_file.rdbuf();
	docker_file.close();
	client_file.close();

	// Copy over stderr data.
	docker_file.open(docker_stderr_path, ios::in);
	client_file.open(stderr_path, ios::out|ios::app);
	client_file << docker_file.rdbuf();
	docker_file.close();
	client_file.close();
    }

    if (rc > 0) {
	clog << docker_build_container_script_path << " failed." << endl;
	return -1;
    }

    // If we're here, we built the container successfully. Now start
    // the container and run stap. First, build up the command line
    // arguments.
    docker_args.clear();
    docker_args.push_back("docker");
    docker_args.push_back("run");
    docker_args.push_back(uuid);
    for (auto it = argv.begin(); it != argv.end(); it++) {
	docker_args.push_back(*it);
    }
    clog << "Running:";
    for (auto it = docker_args.begin(); it != docker_args.end(); it++) {
	clog << " " << *it;
    }
    clog << endl;

    // Set up grabbing the output.
    rc = posix_spawn_file_actions_init(&actions);
    if (rc == 0) {
	rc = posix_spawn_file_actions_addopen(&actions, 0, "/dev/null",
					      O_RDONLY, S_IRWXU);
    }
    if (rc == 0) {
	rc = posix_spawn_file_actions_addopen(&actions, 1,
					      stdout_path.c_str(),
					      O_WRONLY|O_CREAT, S_IRWXU);
    }
    if (rc == 0) {
	rc = posix_spawn_file_actions_addopen(&actions, 2,
					      stderr_path.c_str(),
					      O_WRONLY|O_CREAT, S_IRWXU);
    }
    if (rc != 0) {
	clog << "posix_spawn_file_actions failed: " << strerror(errno)
	     << endl;
	return rc;
    }

    pid = stap_spawn(2, docker_args, &actions);
    clog << "spawn returned " << pid << endl;

    // If stap_spawn() failed, no need to wait.
    if (pid == -1) {
	rc = errno;
	clog << "Error in spawn: " << strerror(errno) << endl;
	(void)posix_spawn_file_actions_destroy(&actions);
	return rc;
    }

    // Wait on the spawned process to finish.
    rc = stap_waitpid(0, pid);
    if (rc < 0) {			// stap_waitpid() failed
	clog << "waitpid failed: " << strerror(errno) << endl;
	(void)posix_spawn_file_actions_destroy(&actions);
	return rc;
    }

    clog << "Spawned process returned " << rc << endl;
    (void)posix_spawn_file_actions_destroy(&actions);

    // Send an error message back in the stderr file.
    ofstream file;
    file.open(stderr_path, ios::out|ios::app);
    file << "Error: Unable to retrieve a module with the docker backend." << endl;;
    file.close();

    return -1;
}

void
get_backends(vector<backend_base *> &backends)
{
    static vector<backend_base *>saved_backends;

    if (saved_backends.empty()) {
	// Note that order *is* important here. We want to try the
	// local backend first (since it would be the fastest), then
	// the docker backend, and finally the default backend (which
	// just returns an error).
	saved_backends.push_back(new local_backend());
	saved_backends.push_back(new docker_backend());
	saved_backends.push_back(new default_backend());
    }
    backends.clear();
    backends = saved_backends;
}
