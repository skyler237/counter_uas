function [ removed_tests ] = removeFaultyTests(test_path, possible_failed_mistakes, possible_success_mistakes )
%UNTITLED Summary of this function goes here
%   Detailed explanation goes here

% test_path = '~/counter_uas/scripts/matlab/skyler/recent_tests/analysis/autotest/test16/test/';

removed_tests = [];
mixed_results = [];


% Handle failed tests
for i=1:size(possible_failed_mistakes,2)
    test_number = num2str(possible_failed_mistakes(1,i));
    bagfile = strcat(test_path,'auto_test', test_number, '.bag');
    results_file = strcat(test_path,'results', test_number, '.bag');
    
    try
        [T, data] = evalc('processAllTopics(bagfile);');
    catch
        if(exist(bagfile,'file'))
            disp(['Removing ' bagfile]);
            system(['rm ' bagfile ' ' results_file]);
            removed_tests = [removed_tests, test_number];
        end        
        continue;
    end
    
    % Check to see if one of the UAV's is "stuck"
    uav1_pose = [data.fleet.uav1.ground_truth.odometry.pose.position];
    uav2_pose = [data.fleet.uav2.ground_truth.odometry.pose.position];
    uav3_pose = [data.fleet.uav3.ground_truth.odometry.pose.position];
    uav4_pose = [data.fleet.uav4.ground_truth.odometry.pose.position];
    intruder_pose = [data.intruder.ground_truth.odometry.pose.position];
    [T, isSuccess, ~, ~, intercept_time] = evalc('findIntercept(bagfile);');
    
    x_ranges = [range(uav1_pose(1,:)), range(uav2_pose(1,:)), range(uav3_pose(1,:)), range(uav4_pose(1,:))];
    
     %  vvv Check for UAV crash   vvv Check for no data   vvv Check for intruder crash    vvv Check for premature results
    if(range(x_ranges) > 30 || size(uav1_pose,2) == 0 || range(intruder_pose(2,:)) == 0 || intercept_time == 0)
        if(exist(bagfile,'file'))
            disp(['Removing ' bagfile]);
            system(['rm ' bagfile ' ' results_file]);
            removed_tests = [removed_tests, test_number];
        end 
    elseif (isSuccess == 1)
        if(exist(bagfile,'file'))        
%             disp(['Test number ' test_number ' has mixed results.']);
            mixed_results = [mixed_results, [str2double(test_number); 0; 1]];
        end
    else
        if(exist(bagfile,'file'))        
%             disp(['NOT removing ' bagfile]);
        end
    end

end

% Handle successful tests
for i=1:size(possible_success_mistakes,2)
    test_number = num2str(possible_success_mistakes(1,i));
    bagfile = strcat(test_path,'auto_test', test_number, '.bag');
    results_file = strcat(test_path,'results', test_number, '.bag');
    
    try
        [T, data] = evalc('processAllTopics(bagfile);');
    catch
        if(exist(bagfile,'file'))
            disp(['Removing ' bagfile]);
            system(['rm ' bagfile ' ' results_file]);
            removed_tests = [removed_tests, test_number];
        end
        continue;
    end
    
    % Check to see if one of the UAV's is "stuck"
    uav1_pose = [data.fleet.uav1.ground_truth.odometry.pose.position];
    uav2_pose = [data.fleet.uav2.ground_truth.odometry.pose.position];
    uav3_pose = [data.fleet.uav3.ground_truth.odometry.pose.position];
    uav4_pose = [data.fleet.uav4.ground_truth.odometry.pose.position];
    intruder_pose = [data.intruder.ground_truth.odometry.pose.position];
    [T, isSuccess, ~, ~, intercept_time] = evalc('findIntercept(bagfile);');
    
    x_ranges = [range(uav1_pose(1,:)), range(uav2_pose(1,:)), range(uav3_pose(1,:)), range(uav4_pose(1,:))];
    
    %  vvv Check for UAV crash   vvv Check for no data   vvv Check for intruder crash    vvv Check for premature results
    if(range(x_ranges) > 30 || size(uav1_pose,2) == 0 || range(intruder_pose(2,:)) == 0 || intercept_time == 0)
        if(exist(bagfile,'file'))
            disp(['Removing ' bagfile]);
            system(['rm ' bagfile ' ' results_file]);
            removed_tests = [removed_tests, test_number];
        end 
    elseif (isSuccess == 0)
        if(exist(bagfile,'file'))        
%             disp(['Test number ' test_number ' has mixed results.']);
            mixed_results = [mixed_results, [str2double(test_number); 1; 0]];
        end
    else
        if(exist(bagfile,'file'))        
%             disp(['NOT removing ' bagfile]);
        end
    end
    
end

    removed_tests = sort(removed_tests);
    
    mixed_results = sortrows(mixed_results')';
    
    if(size(mixed_results,1) > 0)
        fprintf('Mixed results: (between concurrent and post-processed results)\n');
        fprintf('Test #:  Sim:  Matlab:\n');
        format shortG
        disp(mixed_results');
    end

