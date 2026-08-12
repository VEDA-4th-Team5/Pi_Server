pipeline {
    agent { label 'pi-native' }

    options {
        buildDiscarder(logRotator(numToKeepStr: '10'))
        timeout(time: 30, unit: 'MINUTES')
    }

    triggers {
        pollSCM('H/5 * * * *')
    }

    stages {
        stage('Build') {
            steps {
                sh 'cmake -S . -B cmake-build -DCMAKE_BUILD_TYPE=Release'
                sh 'cmake --build cmake-build -j2'
            }
        }
        stage('Test') {
            steps {
                sh 'ctest --test-dir cmake-build --output-on-failure'
            }
        }
        stage('Deploy') {
            when {
                expression { env.GIT_BRANCH == 'origin/main' }
            }
            steps {
                sh 'cp cmake-build/pi-server /home/hun/Pi_server_develop/cmake-build/pi-server'
                sh 'sudo systemctl restart pi-server'
                sh 'sleep 3 && sudo systemctl status pi-server'
            }
        }
    }
}