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
                expression {
                    env.GIT_BRANCH == 'origin/main' ||
                    env.GIT_BRANCH == 'origin/release' ||
                    env.GIT_BRANCH.startsWith('origin/release/')
                }
            }
            steps {
                // 대상 바이너리가 실행 중이면 직접 덮어쓰다 "Text file busy"로
                // 실패한다. 같은 파일시스템의 임시 파일로 복사 후 원자적으로
                // rename하면 실행 중인 프로세스를 건드리지 않고 교체된다.
                sh '''
                    dest=/home/vedaproject/VEDA_FINAL_PROJECT/Pi_Server_develop/cmake-build/pi-server
                    cp cmake-build/pi-server "$dest.new"
                    mv -f "$dest.new" "$dest"
                '''
                sh 'sudo systemctl restart pi-server'
                sh 'sleep 3 && sudo systemctl status pi-server'
            }
        }
    }
}